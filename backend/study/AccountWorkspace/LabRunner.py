"""Run the Yantu gateway and service on fresh loopback-only Linux ports."""
import argparse
from contextlib import contextmanager, ExitStack
from http.client import HTTPConnection, HTTPException, HTTPResponse
import ipaddress
import json
import os
from pathlib import Path
import signal
import secrets
import socket
import ssl
import subprocess
import sys
import threading
from urllib.parse import urlsplit

from backend.study.AccountWorkspace.FrontendSnapshot import FrontendSnapshot
from backend.study.AccountWorkspace.GatewayConfig import add_workspace
from backend.study.AccountWorkspace.AccountWorkspace import origin, prepare_data_directory
from tools.observability.gateway import add_upstream_tls_arguments, check_ports, render, upstream_tls_arguments


ROOT = Path(__file__).resolve().parents[3]
DAILY_AUTHORITY_MARKER = '.nebula-yantu-daily-authority'
ASSISTANT_ASSETS = ('asset-manifest.json', 'assistant/index.html',
                    'shared/PersonalAgent/AgentTurnDag.js', 'shared/PersonalAgent/ContextWindow.js',
                    'shared/AgentRuntime/DagExecutor.js')


def probe(port, path, *, limit=0):
    connection = HTTPConnection('127.0.0.1', port, timeout=2)
    try:
        connection.request('GET', path)
        response = connection.getresponse()
        return response.status, response.read(limit) if limit else b''
    finally:
        connection.close()


@contextmanager
def owned_child(command, log_path, env):
    with log_path.open('xb') as log:
        child = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            yield child
        finally:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)


def wait_ready(child, port, checks, stopped):
    for _ in range(60):
        if stopped.is_set() or child.poll() is not None:
            raise RuntimeError('隔离进程未启动，请查看本轮工作目录中的日志')
        try:
            if all(probe(port, path, limit=8192)[0] == expected for path, expected in checks):
                return
        except (OSError, HTTPException):
            pass
        stopped.wait(.25)
    raise RuntimeError('隔离入口检查超时，请查看本轮工作目录中的日志')


def daily_workspace(directory):
    root = Path(os.path.abspath(directory.expanduser()))
    binding = root / DAILY_AUTHORITY_MARKER
    if (root.exists() and not binding.is_file()
            and any(item.name != '.nebula-yantu-accounts' for item in root.iterdir())):
        raise ValueError('日常账号工作区需要已绑定的目录或新的空目录')
    root = prepare_data_directory(root)
    if binding.is_symlink():
        raise ValueError('账号库绑定文件不能是链接')
    if not binding.exists():
        with binding.open('xb') as output:
            output.write(b'phase11\n')
    if binding.read_bytes() != b'phase11\n':
        raise ValueError('研途工作区未绑定到日常账号库')
    return root


def verify_assistant_routes(port, frontend):
    for relative in ASSISTANT_ASSETS:
        route = '/phase11/assistant/' if relative == 'assistant/index.html' else '/phase11/' + relative
        expected = (frontend / relative).read_bytes()
        status, actual = probe(port, route, limit=len(expected) + 1)
        if status != 200 or actual != expected:
            raise RuntimeError(f'网关未读取本次发布的资源：{route}')
    for route, expected in (('/phase11/assistant', 307), ('/api/ai/web/models', 401),
                            ('/api/ai/private/models', 403), ('/api/local/session/info', 404)):
        if probe(port, route)[0] != expected:
            raise RuntimeError(f'网关路由未返回预期状态：{route}')


def verify_assistant_tls(listen, port, origins, certificate, upstream_enabled):
    origin = next(urlsplit(value) for value in origins if value.startswith('https://'))
    context = ssl.create_default_context(cafile=str(certificate))
    if hasattr(ssl, 'VERIFY_X509_PARTIAL_CHAIN'):
        context.verify_flags |= ssl.VERIFY_X509_PARTIAL_CHAIN
    target = str(listen) if not listen.is_unspecified else '127.0.0.1'
    with socket.create_connection((target, port), timeout=4) as connection:
        with context.wrap_socket(connection, server_hostname=origin.hostname) as stream:
            stream.sendall((f'GET /api/ai/private/models HTTP/1.1\r\nHost: {origin.netloc}\r\n'
                            'Connection: close\r\n\r\n').encode('ascii'))
            response = HTTPResponse(stream)
            try:
                response.begin()
                if response.status != (401 if upstream_enabled else 503):
                    raise RuntimeError(f'HTTPS 私人模型映射未通过匿名鉴权检查，状态 {response.status}')
            finally:
                response.close()


def start(args):
    ports = [args.study_port, args.gateway_port, args.admin_port]
    if args.https_port is not None:
        ports.append(args.https_port)
    elif args.certificate or args.private_key or not args.listen.is_loopback or args.public_origin:
        raise ValueError('请配置 HTTPS 端口和证书后再启用公网域或指定浏览器来源')
    if args.https_port is not None and not all(path and path.is_file() for path in (args.certificate,args.private_key)):
        raise ValueError('HTTPS 需要已有的证书及私钥')
    if args.https_port is not None and not any(value.startswith('https://') for value in args.public_origin):
        raise ValueError('请将客户端使用的 HTTPS 地址列入 --public-origin')
    check_ports(ports if args.daily_authority else [args.upstream_port, *ports])
    if args.daily_authority:
        if args.upstream_port != 18090 or args.accounts_dir is None:
            raise ValueError('日常账号模式需要 18090 上游和单独的持久化账号目录')
        code, raw = probe(args.upstream_port, '/health/version', limit=8192)
        stamp = json.loads(raw) if code == 200 else {}
        if stamp.get('schemaVerifiedAtStartup') is not True or stamp.get('requiredMigration', 0) < 26:
            raise RuntimeError('日常账号上游尚未完成当前数据库迁移检查')
    else:
        if args.accounts_dir is not None:
            raise ValueError('隔离账号模式在本轮工作目录内创建存储')
    envoy = args.envoy.resolve(strict=True)
    if not envoy.is_file() or not os.access(envoy, os.X_OK):
        raise ValueError('指定的 Envoy 程序不可执行')
    frontend = ROOT / 'frontend' / 'phase11'
    FrontendSnapshot(frontend)
    if probe(args.upstream_port, '/health/live')[0] != 200 or probe(args.upstream_port, '/api/me')[0] != 401:
        raise RuntimeError('隔离 NEBULA 上游未通过健康或匿名鉴权检查')
    folder = Path(os.path.abspath(args.work_dir.expanduser()))
    if any(path.is_symlink() for path in (folder, *folder.parents)) or folder.is_relative_to(ROOT):
        raise ValueError('请在部署源码目录之外指定新的真实工作目录')
    os.umask(0o077)
    accounts = daily_workspace(args.accounts_dir) if args.daily_authority else folder / 'accounts'
    folder.mkdir(mode=0o700, parents=True, exist_ok=args.resume)
    origin = f'http://127.0.0.1:{args.gateway_port}'
    public_origins = list(dict.fromkeys([origin, *args.public_origin]))
    configuration = add_workspace(render(args.gateway_port, args.admin_port, args.upstream_port,
                                         folder / 'access.jsonl', https_port=args.https_port, cert=args.certificate,
                                         key=args.private_key, owns_upstream=not args.daily_authority,
                                         **upstream_tls_arguments(args)),
                                  args.study_port, public_origins, serve_frontend=True)
    for listener in configuration['static_resources']['listeners']:
        if listener['name'] == 'nebula_https':
            listener['address']['socket_address']['address'] = str(args.listen)
    config = folder / 'envoy.json'
    if args.resume:
        if config.is_symlink() or json.loads(config.read_text(encoding='utf-8')) != configuration:
            raise RuntimeError('原网关配置与恢复参数不相符')
        suffix = '-resume-' + secrets.token_hex(4)
    else:
        config.write_text(json.dumps(configuration, indent=2) + '\n', encoding='utf-8')
        suffix = ''
    env = {key: value for key, value in os.environ.items() if key not in {'PYTHONPATH', 'PYTHONHOME'}}
    command = [str(envoy), '-c', str(config), '--disable-hot-restart', '--concurrency', '2', '-l', 'warning']
    with (folder / f'envoy-validation{suffix}.log').open('xb') as log:
        subprocess.run([*command, '--mode', 'validate'], cwd=ROOT, check=True, timeout=15,
                       stdout=log, stderr=subprocess.STDOUT, env=env)
    stopped = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stopped.set())
    with ExitStack() as stack:
        study = stack.enter_context(owned_child([
            sys.executable, '-B', '-s', '-m', 'backend.study.AccountWorkspace.AccountWorkspace',
            '--auth-port', str(args.upstream_port), '--port', str(args.study_port),
            *[item for value in public_origins for item in ('--public-origin', value)],
            '--data-dir', str(accounts), '--frontend-root', str(frontend),
        ], folder / f'study{suffix}.log', env))
        wait_ready(study, args.study_port, (('/api/study/session', 401),), stopped)
        gateway = stack.enter_context(owned_child(command, folder / f'envoy{suffix}.log', env))
        wait_ready(gateway, args.gateway_port, (('/health/live', 200), ('/phase11/study/', 200),
                                               ('/api/study/session', 401)), stopped)
        if b'id="study-surface"' not in probe(args.gateway_port, '/phase11/study/', limit=8192)[1]:
            raise RuntimeError('访问到了页面，但未确认使用本轮的研途前端')
        verify_assistant_routes(args.gateway_port, frontend)
        if args.https_port:
            verify_assistant_tls(args.listen, args.https_port, public_origins, args.certificate,
                                 getattr(args, 'upstream_tls_port', None) is not None)
        print(f'研途隔离入口 {origin}/phase11/study/', flush=True)
        if not args.check_only:
            while not stopped.wait(.5):
                if study.poll() is not None or gateway.poll() is not None:
                    raise RuntimeError('隔离进程提前退出，请检查本轮日志')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--envoy', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, required=True)
    parser.add_argument('--upstream-port', type=int, default=18094)
    parser.add_argument('--study-port', type=int, default=18772)
    parser.add_argument('--gateway-port', type=int, default=18095)
    parser.add_argument('--admin-port', type=int, default=19097)
    parser.add_argument('--https-port', type=int, help='Optional TLS listener for Windows Companion clients')
    parser.add_argument('--listen', type=ipaddress.IPv4Address, default=ipaddress.IPv4Address('127.0.0.1'))
    parser.add_argument('--certificate', type=Path)
    parser.add_argument('--private-key', type=Path)
    add_upstream_tls_arguments(parser)
    parser.add_argument('--public-origin', action='append', type=origin, default=[], help='HTTPS origin used by the client')
    parser.add_argument('--daily-authority', action='store_true', help='Use the migrated daily service as the account and business authority')
    parser.add_argument('--accounts-dir', type=Path, help='Persistent account directory for daily authority')
    parser.add_argument('--resume', action='store_true', help='Restart a matching prior gateway without replacing its workspace')
    parser.add_argument('--check-only', action='store_true', help='Probe the stack then shut down the two owned processes')
    args = parser.parse_args()
    if not sys.platform.startswith('linux'):
        parser.error('在 Linux 的隔离环境运行本入口')
    try:
        start(args)
    except KeyboardInterrupt:
        pass
    except (ValueError, RuntimeError, OSError, HTTPException, subprocess.SubprocessError) as error:
        parser.exit(1, f'研途隔离启动未完成：{error}\n')


if __name__ == '__main__':
    main()
