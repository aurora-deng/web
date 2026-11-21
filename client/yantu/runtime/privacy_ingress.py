"""Loopback-only streaming ingress that never forwards peer identity to the app."""

import http.client
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

HOP = {'connection', 'keep-alive', 'proxy-authenticate', 'proxy-authorization', 'te', 'trailer', 'transfer-encoding', 'upgrade'}
METADATA = {'/api/info', '/api/health', '/api/protection', '/api/assistant/status'}


def phone_metadata(path, payload, origin):
    """Keep older backends from exposing local configuration through the relay."""
    if not isinstance(payload, dict):
        raise ValueError('Invalid metadata response')
    if path == '/api/info':
        payload = {key: payload[key] for key in ('files', 'bytes', 'revision') if key in payload}
        payload.update({'local': False, 'urls': [], 'code': None, 'dataPath': None,
                        'remote': {'active': True, 'kind': 'private', 'url': origin, 'available': True, 'error': None}})
    elif path == '/api/health':
        payload = {key: payload[key] for key in ('app', 'version') if key in payload}
        payload['dataPath'] = None
    elif path == '/api/protection':
        payload['local'] = False
        secondary = payload.get('config', {}).get('secondary', {})
        if isinstance(secondary, dict):
            secondary.pop('path', None)
        for field in ('backup', 'secondary'):
            detail = payload.get(field)
            if isinstance(detail, dict):
                detail.pop('path', None)
                if detail.get('error'):
                    detail['error'] = '备份未成功，请到电脑端查看详情'
    elif path == '/api/assistant/status':
        payload['local'] = False
        payload.pop('ollamaUrl', None)
        payload.pop('apiUrl', None)
        if isinstance(payload.get('ollama'), dict) and payload['ollama'].get('error'):
            payload['ollama']['error'] = '电脑本地模型暂不可用'
    return payload


def safe_origin(value):
    parsed = urlsplit(value)
    if parsed.scheme != 'https' or not parsed.hostname or not parsed.hostname.endswith('.ts.net') or parsed.port not in {None, 443} or parsed.username or parsed.password or parsed.path not in {'', '/'} or parsed.query or parsed.fragment:
        raise ValueError('私有入口必须是当前设备的 Tailscale HTTPS 地址')
    return 'https://' + parsed.hostname


class PrivacyIngress(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, port, upstream_port, origin):
        self.origin = safe_origin(origin)
        self.upstream_port = upstream_port
        super().__init__(('127.0.0.1', port), IngressHandler)
        self.thread = threading.Thread(target=self.serve_forever, name='yantu-private-ingress', daemon=True)
        self.thread.start()

    def close(self):
        self.shutdown()
        self.server_close()


class IngressHandler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'Yantu'
    sys_version = ''

    def log_message(self, *_):
        pass

    def _forward(self):
        connection = None
        sent = False
        self.close_connection = True
        try:
            if self.client_address[0] != '127.0.0.1' or not self.path.startswith('/') or self.path.startswith('//') or self.headers.get('Transfer-Encoding'):
                self.send_error(400, 'Unsupported request')
                return
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 <= length <= 20 * 1024 ** 3:
                self.send_error(413, 'Request is too large')
                return
            connection = http.client.HTTPConnection('127.0.0.1', self.server.upstream_port, timeout=300)
            path = urlsplit(self.path).path
            native = self.command == 'GET' and path == '/api/native/plan'
            connection.putrequest(self.command, self.path, skip_host=True, skip_accept_encoding=True)
            connection.putheader('Host', urlsplit(self.server.origin).netloc)
            blocked = HOP | {name.strip().lower() for name in self.headers.get('Connection', '').split(',')}
            for key, value in self.headers.items():
                lowered = key.lower()
                if lowered in blocked or lowered in {'host', 'forwarded', 'x-yantu-relay', 'user-agent', 'via', 'x-real-ip', 'client-ip', 'true-client-ip', 'x-client-ip'} or lowered.startswith(('tailscale-', 'x-forwarded-', 'cf-', 'sec-ch-ua', 'x-device-')):
                    continue
                connection.putheader(key, value)
            connection.putheader('User-Agent', 'YantuPrivate/1')
            connection.putheader('X-Yantu-Relay', '1')
            connection.putheader('X-Forwarded-Proto', 'https')
            connection.putheader('Connection', 'close')
            connection.endheaders()
            remaining = length
            while remaining:
                chunk = self.rfile.read(min(65536, remaining))
                if not chunk:
                    raise ValueError('Upload interrupted')
                connection.send(chunk)
                remaining -= len(chunk)
            response = connection.getresponse()
            filtered = None
            if self.command != 'HEAD' and path.startswith('/api/') and (((path in METADATA or native) and 200 <= response.status < 300) or response.status >= 500) and 'application/json' in response.getheader('Content-Type', ''):
                maximum = 4 * 1024 ** 2
                raw = response.read(maximum + 1)
                if len(raw) > maximum:
                    raise ValueError('Metadata response too large')
                if native and 200 <= response.status < 300:
                    payload = json.loads(raw)
                    if payload.get('format') != 'yantu-native-plan-v1':
                        raise ValueError('Unexpected native plan format')
                else:
                    payload = {'error': '电脑服务暂不可用，请在电脑端查看详情'} if response.status >= 500 else phone_metadata(path, json.loads(raw), self.server.origin)
                filtered = json.dumps(payload, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
            self.send_response_only(response.status)
            for key, value in response.getheaders():
                if key.lower() not in HOP | {'server', 'x-powered-by', 'x-runtime'} and not key.lower().startswith('tailscale-') and not (filtered is not None and key.lower() in {'content-length', 'etag', 'content-encoding'}):
                    self.send_header(key, value)
            if filtered is not None:
                self.send_header('Content-Length', str(len(filtered)))
            self.send_header('Connection', 'close')
            self.end_headers()
            sent = True
            if filtered is not None:
                self.wfile.write(filtered)
            elif self.command != 'HEAD':
                while chunk := response.read1(65536):
                    self.wfile.write(chunk)
                    self.wfile.flush()
        except (OSError, ValueError, http.client.HTTPException):
            if not sent:
                body = b'{"error":"Private learning service is temporarily unavailable"}'
                try:
                    self.send_response_only(502)
                    self.send_header('Content-Type', 'application/json')
                    self.send_header('Content-Length', str(len(body)))
                    self.send_header('Connection', 'close')
                    self.end_headers()
                    self.wfile.write(body)
                except OSError:
                    pass
        finally:
            if connection:
                connection.close()

    do_GET = do_HEAD = do_POST = do_PUT = do_DELETE = do_OPTIONS = _forward
