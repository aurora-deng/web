"""Serve account-scoped Yantu workspaces behind the NEBULA gateway."""
import argparse
from collections import OrderedDict
from contextlib import contextmanager
import hashlib
from http.client import HTTPConnection, HTTPException
from http.cookies import SimpleCookie, CookieError
import json
import mimetypes
import os
from pathlib import Path
import re
import secrets
import sys
from threading import BoundedSemaphore, Lock
from urllib.parse import urlsplit

from backend.study.AccountWorkspace import UploadParts
from backend.study.AccountWorkspace.FrontendSnapshot import FRONTEND_PREFIX, FrontendSnapshot, MAX_ASSET_BYTES


PREFIX = '/api/study/'
ROOT = Path(__file__).resolve().parents[3]
RUNTIME = ROOT / 'client' / 'yantu' / 'runtime'
WEB = RUNTIME / 'target'
MARKER = b'nebula-yantu-account-workspaces-v1\n'
MAX_UPLOAD = 10 * 1024 * 1024
MAX_STORAGE = 512 * 1024 * 1024
ACCOUNT = re.compile(r'[1-9][0-9]{0,19}')
SCOPE = re.compile(r'[0-9a-f]{32}')
ASSET_PATH = re.compile(r'/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*')
FILES = re.compile(r'/api/files/[A-Za-z0-9_-][A-Za-z0-9_.-]{0,99}')
STATIC_TYPES = {'.mjs': 'text/javascript', '.wasm': 'application/wasm',
                '.webmanifest': 'application/manifest+json'}
NON_ASSETS = {'.html', '.php', '.py'}

READ = {
    '/api/state', '/api/time', '/api/native/plan', '/api/protection', '/api/protection/backup-file',
    '/api/recovery', '/api/workspace/capabilities', '/api/workspace/file-version', '/api/workspace/cleanup',
    '/api/workspace/search', '/api/workspace/assistant-job', '/api/planning/focus', '/api/notifications',
    '/api/assistant/status', '/api/assistant/snapshot', '/api/assistant/prompt', '/api/assistant/attachment',
    '/api/assistant/memories', '/api/assistant/conversations', '/api/assistant/messages', '/api/assistant/job',
    '/api/media/page', '/api/media/export', '/api/export',
}
WRITE = {
    '/api/sync', '/api/planning/preview', '/api/planning/apply', '/api/planning/focus',
    '/api/workspace/assistant-preview', '/api/workspace/assistant-confirm', '/api/workspace/assistant-undo',
    '/api/workspace/assistant-generate', '/api/workspace/assistant-cancel',
    '/api/workspace/exam-save', '/api/workspace/exam-preview', '/api/workspace/exam-confirm',
    '/api/workspace/knowledge-preview', '/api/workspace/knowledge-merge', '/api/workspace/knowledge-undo',
    '/api/workspace/cleanup', '/api/workspace/reading-export', '/api/protection/resolve', '/api/protection/backup',
    '/api/recovery/restore', '/api/recovery/purge', '/api/reset-study',
    '/api/import', '/api/import/preview', '/api/import/confirm', '/api/import/cancel',
    '/api/assistant/device-sync', '/api/assistant/chat', '/api/assistant/cancel', '/api/assistant/memory', '/api/assistant/forget',
    '/api/assistant/delete-conversation',
}


def prepare_data_directory(path):
    absolute = Path(os.path.abspath(path.expanduser()))
    for part in (absolute, *absolute.parents):
        if part.is_symlink() or (hasattr(part, 'is_junction') and part.is_junction()):
            raise ValueError('数据目录不能经过符号链接')
    if absolute == absolute.parent or absolute.is_relative_to(RUNTIME):
        raise ValueError('请选择研途账号服务专用目录')
    absolute.mkdir(mode=0o700, parents=True, exist_ok=True)
    flag = absolute / '.nebula-yantu-accounts'
    if flag.exists():
        if flag.is_symlink() or flag.read_bytes() != MARKER:
            raise ValueError('研途账号工作区标记不匹配')
    elif any(absolute.iterdir()):
        raise ValueError('请使用新的空目录，原研途资料目录无法用作账号服务')
    else:
        with flag.open('xb') as output:
            output.write(MARKER)
    return absolute


def restore_session(cookie, port):
    if not cookie or len(cookie) > 4096:
        return None
    try:
        parsed = SimpleCookie(cookie)
    except CookieError:
        return None
    entry = parsed.get('phase11_session')
    if not entry or not re.fullmatch(r'[A-Za-z0-9._~+=/-]{16,1024}', entry.value):
        return None
    connection = HTTPConnection('127.0.0.1', port, timeout=2.0)
    try:
        connection.request('GET', '/api/me', headers={'Cookie': f'phase11_session={entry.value}'})
        response = connection.getresponse()
        if response.status in (401, 403):
            return None
        if response.status != 200:
            raise RuntimeError('NEBULA 账号服务暂不可用')
        payload = json.loads(response.read(16385))
        identity = str(payload['user']['id'])
        csrf = payload['csrfToken']
        if not ACCOUNT.fullmatch(identity) or not isinstance(csrf, str) or not 16 <= len(csrf) <= 512:
            raise ValueError('invalid session response')
        return identity, csrf
    except (HTTPException, OSError, UnicodeError, ValueError, KeyError, TypeError) as error:
        raise RuntimeError('NEBULA 账号服务暂不可用') from error
    finally:
        connection.close()


class AccountServices:
    def __init__(self, root, runtime, max_active, model, model_port):
        self.runtime = runtime
        self.root = root
        self.max_active = max_active
        self.model = model
        self.model_port = model_port
        self.active = OrderedDict()
        self.lock = Lock()
        key_file = root / 'workspace-scope.key'
        if key_file.is_symlink():
            raise ValueError('工作区密钥不能是链接')
        try:
            with key_file.open('xb') as output:
                output.write(secrets.token_bytes(32))
        except FileExistsError:
            pass
        self.key = key_file.read_bytes()
        if len(self.key) != 32:
            raise ValueError('工作区密钥无效')

    def scope(self, account):
        return hashlib.blake2s(account.encode('ascii'), key=self.key, digest_size=16).hexdigest()

    def bind(self, account):
        """Every verified NEBULA identity has one stable workspace in this authority."""
        key = self.scope(account)
        directory = self.root / key
        if directory.is_symlink():
            raise RuntimeError('研途账号存储暂不可用')
        directory.mkdir(mode=0o700, exist_ok=True)
        return key

    @contextmanager
    def use(self, key):
        services = None
        with self.lock:
            if key not in self.active and len(self.active) >= self.max_active:
                idle = next((name for name, (_, refs) in self.active.items() if refs == 0), None)
                if idle is not None:
                    old, _ = self.active.pop(idle)
                    old.close()
            if key in self.active or len(self.active) < self.max_active:
                if key not in self.active:
                    data = self.root / key
                    data.mkdir(mode=0o700, exist_ok=True)
                    if data.is_symlink() or any(child.is_symlink() for child in data.rglob('*')):
                        raise RuntimeError('研途账号存储暂不可用')
                    services = self.runtime.WorkspaceServices(data, 0, hosted=True, restore_limit=MAX_STORAGE)
                    services.assistant.config.update(provider='ollama', ollamaUrl=f'http://127.0.0.1:{self.model_port}',
                                                     ollamaModel=self.model, apiUrl='', apiModel='', protectedKey='', cloudConsent=False)
                    self.active[key] = services, 0
                services, refs = self.active[key]
                self.active[key] = services, refs + 1
                self.active.move_to_end(key)
        try:
            yield services
        finally:
            if services is not None:
                with self.lock:
                    current, refs = self.active[key]
                    self.active[key] = current, refs - 1

    def close(self):
        for services, _ in self.active.values():
            services.close()
        self.active.clear()


def inline_media(mime):
    return mime in {'application/pdf', 'text/plain', 'image/png', 'image/jpeg', 'image/webp',
                    'image/gif', 'image/avif', 'image/bmp'} or bool(re.fullmatch(
                        r'(audio|video)/[A-Za-z0-9.+-]+', mime))


def run(args):
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(RUNTIME))
    import server as runtime
    from desktop_runtime import SupervisorLease
    allowed_origins = {origin_key(value) for value in args.public_origin}
    frontend = FrontendSnapshot(args.frontend_root) if args.frontend_root else None
    from backend.study.AccountWorkspace import AgentControl
    from backend.agent.tool_runtime.control import ControlStore
    control_enabled = os.environ.get('WEB_AGENT_CONTROL_ENABLE') == '1'
    control_fleet = control_bridge = None
    if control_enabled and getattr(args, 'control_fleet_ctl', None):
        from backend.agent.tool_runtime.workspace import NativeFleetPort
        from backend.agent.tool_runtime.skills import FleetSimulationBridge
        control_fleet = NativeFleetPort(args.control_fleet_ctl, args.control_fleet_address, None, args.control_checkpoint)
        control_bridge = FleetSimulationBridge(args.control_fleet_agent, args.control_fleet_address, None)

    class HostedHandler(runtime.Handler):
        def setup(self):
            self.request.settimeout(18)
            super().setup()

        @property
        def services(self):
            return self.account_services

        @property
        def is_pc(self):
            return True  # Account owner inside the hosted workspace, after route authorization.

        def cors_origin(self):
            return None

        def require_auth(self):
            return True  # Auth happens before the original handlers run.

        def same_origin(self):
            return True  # Mutations are checked against the approved origin and NEBULA CSRF token.

        def planning_owner(self):
            return 'nebula-account'  # The WorkspaceServices object belongs to exactly one account.

        def restore_owner(self):
            return self.restore_session_key

        def log_message(self, fmt, *args):
            return  # Account paths, auth headers, and query values are private.

        def end_headers(self):
            self.send_header('Connection', 'close')
            self.send_header('Content-Security-Policy', "object-src 'none'; base-uri 'self'; frame-ancestors 'self'")
            self.send_header('X-Frame-Options', 'SAMEORIGIN')
            self.send_header('Referrer-Policy', 'same-origin')
            self.send_header('X-Content-Type-Options', 'nosniff')
            super().end_headers()

        def send_header(self, keyword, value):
            if keyword.lower() == 'content-type' and FILES.fullmatch(urlsplit(self.path).path):
                mime = value.split(';', 1)[0].lower()
                if not inline_media(mime):
                    value = 'application/octet-stream'
            super().send_header(keyword, value)

        def reply(self, status, value, headers=None, mime='application/json; charset=utf-8'):
            if status >= 500:
                value, mime = {'error': '研途工作区暂不可用，请重试'}, 'application/json; charset=utf-8'
            super().reply(status, value, headers, mime)

        def do_GET(self):
            self.dispatch()

        do_HEAD = do_POST = do_PUT = do_DELETE = do_GET

        def dispatch(self):
            self.close_connection = True
            changed = self.command in ('POST', 'PUT', 'DELETE')
            origin = self.headers.get('Origin')
            if self.headers.get('Sec-Fetch-Site') == 'cross-site' or (origin and origin_key(origin) not in allowed_origins):
                return self.reply(403, {'error': '请从 NEBULA 工作区访问研途'})
            if changed:
                lengths = self.headers.get_all('Content-Length', [])
                if (self.headers.get('Transfer-Encoding') or len(lengths) > 1 or
                        (lengths and not re.fullmatch(r'[0-9]{1,9}', lengths[0]))):
                    return self.reply(400, {'error': '请求长度无效'})
                if lengths and int(lengths[0]) > MAX_UPLOAD:
                    return self.reply(413, {'error': '单次请求上限为 10 MiB'})
            try:
                target = urlsplit(self.path)
                if AgentControl.dispatch(self, args, target, self.server.accounts,
                                         self.server.control, control_fleet, control_bridge,
                                         allowed_origins, origin_key):
                    return
                if frontend is not None and not target.netloc and not target.scheme and (
                    target.path == FRONTEND_PREFIX.rstrip('/') or target.path.startswith(FRONTEND_PREFIX)
                ):
                    return self.frontend_asset(frontend, target.path)
                if target.netloc or target.scheme or not target.path.startswith(PREFIX):
                    return self.reply(404, {'error': '未知研途路径'})
                suffix = target.path[len(PREFIX):]
                key, slash, resource = suffix.partition('/')
                path = '/' + resource
                valid_key = bool(slash and SCOPE.fullmatch(key))
                if suffix != 'session' and not valid_key:
                    return self.reply(404, {'error': '未知研途路径'})
                if (valid_key and not changed and not path.startswith('/api/') and
                        path not in {'/', '/index.html', '/mobile', '/mobile/'}):
                    return self.asset(path)
                session = restore_session(self.headers.get('Cookie'), args.auth_port)
                if session is None:
                    return self.reply(401, {'error': '请先登录 NEBULA'})
                identity, csrf = session
                self.restore_session_key = hashlib.sha256(csrf.encode()).hexdigest()
                if suffix == 'session' and not changed:
                    key = self.server.accounts.bind(identity)
                    return self.reply(200, {'accountId': identity, 'scope': key,
                                            'url': f'{PREFIX}{key}/', 'csrfToken': csrf}, {'Cache-Control': 'no-store'})
                expected = self.server.accounts.scope(identity)
                if not valid_key or not secrets.compare_digest(expected, key):
                    return self.reply(403, {'error': '研途工作区与当前账号不匹配，请重新打开研途'},
                                      {'X-Study-Session': 'changed'})
                if changed and (not origin or not secrets.compare_digest(csrf.encode(), self.headers.get('X-CSRF-Token', '').encode())):
                    return self.reply(403, {'error': '账号会话已变化，请重新打开研途'},
                                      {'X-Study-Session': 'changed'})
                if path in {'/', '/index.html', '/mobile', '/mobile/'} and not changed:
                    return self.asset('/index.html', shell=True)
                if path in {'/api/info', '/api/health'} and not changed:
                    return self.reply(200, {'app': 'nebula-yantu', 'mode': 'account', 'local': True,
                                            'urls': [], 'remote': {}, 'version': 3})
                if not self.allowed(path):
                    return self.reply(403, {'error': '此操作需要 Windows 研途本机能力'})
                self.path = path + ('?' + target.query if target.query else '')
                with self.server.accounts.use(key) as services:
                    if services is None:
                        return self.reply(503, {'error': '研途工作区繁忙，请稍后重试'})
                    self.account_services = services
                    if self.command == 'PUT':
                        if self.headers.get('Content-Range'):
                            return UploadParts.receive(self, runtime, MAX_STORAGE)
                        with services.storage.lock:
                            stored, _ = UploadParts.room_for(services.storage, path.rsplit('/', 1)[-1])
                            if stored + int(self.headers.get('Content-Length', '0')) > MAX_STORAGE:
                                return self.reply(413, {'error': '研途学习库附件已达到 512 MiB'})
                            return runtime.Handler.do_PUT(self)
                    return getattr(runtime.Handler, 'do_' + ('GET' if self.command == 'HEAD' else self.command))(self)
            except (BrokenPipeError, ConnectionError, TimeoutError):
                pass
            except (RuntimeError, OSError, ValueError):
                self.reply(503, {'error': '研途工作区暂不可用，请重试'})

        def allowed(self, path):
            if FILES.fullmatch(path):
                return True
            if self.command in ('GET', 'HEAD'):
                return path in READ
            return self.command == 'POST' and path in WRITE

        def asset(self, path, shell=False):
            if not ASSET_PATH.fullmatch(path):
                return self.reply(404, {'error': '研途资源不存在'})
            file = (WEB / path.lstrip('/')).resolve()
            if (not file.is_relative_to(WEB) or not file.is_file() or
                    (file.suffix in NON_ASSETS and not shell)):
                return self.reply(404, {'error': '研途资源不存在'})
            mime = STATIC_TYPES.get(file.suffix) or mimetypes.guess_type(file.name)[0] or 'application/octet-stream'
            self.reply(200, file.read_bytes(), mime=mime)

        def frontend_asset(self, snapshot, path):
            if self.command not in ('GET', 'HEAD'):
                return self.reply(405, {'error': '页面仅支持读取'})
            selected = snapshot.asset(path)
            if not selected:
                return self.reply(404, {'error': '页面不存在'})
            file, mime = selected
            with file.open('rb') as content:
                body = content.read(MAX_ASSET_BYTES + 1)
            if len(body) > MAX_ASSET_BYTES:
                return self.reply(413, {'error': '前端资源超过大小限制'})
            self.reply(200, body, mime=mime)

    class AccountServer(runtime.ThreadingHTTPServer):
        request_queue_size = 64
        daemon_threads = False

        def __init__(self, address, handler):
            super().__init__(address, handler)
            self.requests = BoundedSemaphore(40)

        def process_request(self, request, address):
            if not self.requests.acquire(blocking=False):
                try:
                    request.settimeout(.25)
                    request.sendall(b'HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n')
                except OSError:
                    pass
                self.shutdown_request(request)
                return
            try:
                super().process_request(request, address)
            except BaseException:
                self.requests.release()
                raise

        def process_request_thread(self, request, address):
            try:
                super().process_request_thread(request, address)
            finally:
                self.requests.release()

        def handle_error(self, request, address):
            print('研途账号服务请求处理失败', file=sys.stderr)

    data = prepare_data_directory(args.data_dir)
    with SupervisorLease(data):
        accounts = AccountServices(data, runtime, args.max_active, args.ollama_model, args.ollama_port)
        try:
            with AccountServer(('127.0.0.1', args.port), HostedHandler) as instance:
                instance.accounts = accounts
                instance.control = ControlStore(data / 'agent-control.sqlite3') if control_enabled else None
                print(f'NEBULA 研途账号服务 127.0.0.1:{instance.server_port}', flush=True)
                instance.serve_forever(poll_interval=.5)
        finally:
            accounts.close()


def origin_key(value):
    try:
        parsed = urlsplit(value)
        if (not parsed.hostname or parsed.username or parsed.password or parsed.path or parsed.query or parsed.fragment or
                (parsed.scheme != 'https' and not (parsed.scheme == 'http' and parsed.hostname in {'127.0.0.1', 'localhost', '::1'}))):
            return None
        default = 443 if parsed.scheme == 'https' else 80
        return parsed.scheme, parsed.hostname, parsed.port or default
    except ValueError:
        return None


def origin(value):
    if origin_key(value) is None:
        raise argparse.ArgumentTypeError('公开入口须为 HTTPS 源，本地验证可用 HTTP 回环地址')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data-dir', required=True, type=Path)
    parser.add_argument('--port', default=18772, type=int)
    parser.add_argument('--auth-port', required=True, type=int, help='NEBULA upstream HTTP port on 127.0.0.1')
    parser.add_argument('--public-origin', required=True, action='append', type=origin)
    parser.add_argument('--max-active', default=16, type=int)
    parser.add_argument('--frontend-root', type=Path, help='Optional isolated web frontend snapshot')
    parser.add_argument('--ollama-model', default='', help='Optional model already installed on the site Ollama')
    parser.add_argument('--ollama-port', default=11434, type=int, help='Fixed loopback model port')
    parser.add_argument('--control-fleet-ctl', type=Path, help='Existing built Fleet read CLI, never downloaded')
    parser.add_argument('--control-fleet-agent', type=Path, help='Existing built simulation approval CLI')
    parser.add_argument('--control-fleet-address', default='127.0.0.1:18126')
    parser.add_argument('--control-checkpoint', type=Path, help='Existing Edge checkpoint projection')
    args = parser.parse_args()
    if any((args.control_fleet_ctl, args.control_fleet_agent, args.control_checkpoint)) and not all((args.control_fleet_ctl, args.control_fleet_agent, args.control_checkpoint)):
        parser.error('Fleet integration requires both existing CLIs and a checkpoint projection')
    if not (0 < args.port < 65536 and 0 < args.auth_port < 65536 and 1 <= args.max_active <= 64 and args.port != args.auth_port):
        parser.error('端口或工作区并发上限无效')
    if not 0 < args.ollama_port < 65536 or (args.ollama_model and not re.fullmatch(r'[A-Za-z0-9_./:-]{1,160}', args.ollama_model)):
        parser.error('Ollama 模型名称或端口无效')
    if os.name != 'nt':
        os.umask(0o077)
    try:
        run(args)
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError) as error:
        raise SystemExit(f'研途账号服务启动失败：{error}') from None


if __name__ == '__main__':
    main()
