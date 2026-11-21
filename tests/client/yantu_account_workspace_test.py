"""Exercise the real account server using synthetic NEBULA sessions and records."""
import argparse
from contextlib import contextmanager
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from urllib.parse import urlsplit


ROOT = Path(__file__).resolve().parents[2]
SESSIONS = {'synthetic_account7_device1': '7', 'synthetic_account7_device2': '7',
            'synthetic_account8_device1': '8'}


def csrf(token):
    return 'synthetic-csrf:' + token


def unused_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def request(port, method, route, token=None, body=None, headers=None):
    fields = dict(headers or {})
    if token:
        fields['Cookie'] = 'phase11_session=' + token
    if isinstance(body, dict):
        body = json.dumps(body).encode()
        fields['Content-Type'] = 'application/json'
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
    try:
        connection.request(method, route, body, fields)
        response = connection.getresponse()
        return response.status, response.read(), dict(response.getheaders())
    finally:
        connection.close()


class GatewayFixture(BaseHTTPRequestHandler):
    def log_message(self, *args):
        return

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        try:
            self.dispatch()
        except (BrokenPipeError, ConnectionError):
            pass

    do_POST = do_PUT = do_DELETE = do_GET

    def response(self, status, body, headers=None):
        headers = dict(headers or {})
        self.send_response(status)
        if 'Content-Length' not in headers:
            headers['Content-Length'] = str(len(body))
        for name, value in headers.items():
            self.send_header(name, value)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def dispatch(self):
        path = urlsplit(self.path).path
        if path == '/api/auth/login' and self.command == 'POST':
            fields = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))))
            if (fields.get('username') not in {'synthetic', 'fixture-second'}
                    or fields.get('password') != 'fixture-only'):
                return self.response(401, b'{"error":"INVALID_CREDENTIALS"}',
                                     {'Content-Type': 'application/json'})
            account = '8' if fields.get('username') == 'fixture-second' else '7'
            token = 'synthetic_account8_device1' if account == '8' else 'synthetic_account7_device2'
            return self.response(200, json.dumps({'user': {'id': account, 'displayName': 'Synthetic'},
                                                  'csrfToken': csrf(token)}).encode(), {
                'Content-Type': 'application/json', 'Set-Cookie': f'phase11_session={token}; Path=/; HttpOnly; Secure'})
        if path == '/api/me':
            token = self.headers.get('Cookie', '').removeprefix('phase11_session=')
            account = self.server.sessions.get(token)
            if not account:
                return self.response(401, b'{}', {'Content-Type': 'application/json'})
            return self.response(200, json.dumps({'user': {'id': account, 'displayName': 'Synthetic'},
                                                  'csrfToken': csrf(token)}).encode(), {'Content-Type': 'application/json'})
        if path == '/phase11' or path.startswith(('/api/study/', '/phase11/')):
            length = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(length) if length else None
            headers = {key: value for key, value in self.headers.items()
                       if key.lower() not in {'connection', 'content-length', 'host', 'accept-encoding'}}
            status, payload, response = request(self.server.study_port, self.command, self.path, body=body, headers=headers)
            return self.response(status, payload, {key: value for key, value in response.items()
                                                  if key.lower() not in {'server', 'date', 'connection'}})
        self.response(404, b'{}', {'Content-Type': 'application/json'})


class FixtureServer(ThreadingHTTPServer):
    request_queue_size = 64


@contextmanager
def scenario(workdir, port=0, max_active=1, tls=None):
    with FixtureServer(('127.0.0.1', port), GatewayFixture) as gateway:
        gateway.sessions = dict(SESSIONS)
        gateway.study_port = unused_port()
        origin = f'http://127.0.0.1:{gateway.server_port}'
        thread = threading.Thread(target=gateway.serve_forever)
        thread.start()
        origins = ['--public-origin', origin]
        gateway.tls_origin = ''
        if tls:
            secure = FixtureServer(('127.0.0.1', 0), GatewayFixture)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(*tls)
            secure.socket = context.wrap_socket(secure.socket, server_side=True)
            secure.sessions, secure.study_port = gateway.sessions, gateway.study_port
            gateway.tls_origin = f'https://localhost:{secure.server_port}'
            secure_thread = threading.Thread(target=secure.serve_forever)
            secure_thread.start()
            origins += ['--public-origin', gateway.tls_origin]
        with (workdir / 'study-stderr.log').open('w+b') as stderr:
            process = subprocess.Popen([sys.executable, '-B', '-m', 'backend.study.AccountWorkspace.AccountWorkspace',
                                        '--auth-port', str(gateway.server_port), '--port', str(gateway.study_port),
                                        *origins, '--max-active', str(max_active),
                                        '--data-dir', str(workdir / 'account-data'),
                                        '--frontend-root', str(ROOT / 'frontend' / 'phase11')], cwd=ROOT,
                                       stdout=subprocess.DEVNULL, stderr=stderr)
            try:
                for _ in range(50):
                    try:
                        ready = request(gateway.study_port, 'GET', '/api/study/session')[0] == 401
                    except OSError:
                        ready = False
                    if ready:
                        break
                    if process.poll() is not None:
                        break
                    time.sleep(.1)
                if not ready:
                    stderr.seek(0)
                    raise RuntimeError('Account server did not start: ' + stderr.read().decode(errors='replace'))
                yield gateway, origin
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                gateway.shutdown()
                thread.join(timeout=5)
                if tls:
                    secure.shutdown()
                    secure_thread.join(timeout=5)
                    secure.server_close()


class AccountWorkspaceTest(unittest.TestCase):
    def test_devices_authors_files_and_revisions(self):
        workspace = ROOT / 'build-function3'
        workspace.mkdir(exist_ok=True)
        first, second, other = SESSIONS
        with tempfile.TemporaryDirectory(prefix='study-account-test-', dir=workspace) as run:
            with scenario(Path(run)) as (gateway, origin):
                port = gateway.study_port
                self.assertEqual(request(port, 'GET', '/api/study/session')[0], 401)
                status, landing, headers = request(port, 'GET', '/phase11/study/')
                self.assertEqual(status, 200)
                self.assertIn(b'id="study-surface"', landing)
                self.assertIn('text/html', headers['Content-Type'])
                self.assertEqual(request(port, 'GET', '/phase11/%2e%2e/client/yantu/runtime/server.py')[0], 404)
                self.assertEqual(request(port, 'POST', '/phase11/study/', body={}, headers={'Origin': origin})[0], 405)

                def json_get(route, who):
                    status, body, _ = request(port, 'GET', route, who)
                    self.assertEqual(status, 200, body)
                    return json.loads(body)

                first_binding = json_get('/api/study/session', first)
                self.assertEqual(first_binding['accountId'], '7')
                self.assertTrue((Path(run) / 'account-data' / first_binding['scope']).is_dir())
                a = first_binding['url']
                self.assertEqual(a, json_get('/api/study/session', second)['url'])
                self.assertEqual(a, json_get('/api/study/session?accountId=8', first)['url'])
                second_binding = json_get('/api/study/session', other)
                self.assertEqual(second_binding['accountId'], '8')
                b = second_binding['url']
                self.assertNotEqual(a, b)
                self.assertEqual(request(port, 'GET', a, other)[0], 403)
                self.assertEqual(request(port, 'GET', a + 'api/state', other)[0], 403)
                self.assertEqual(request(port, 'GET', a + 'api/obsidian/config', first)[0], 403)
                self.assertFalse(json_get(a + 'api/state', first)['state'].get('notes'))
                op = {'opId': 'fixture-first', 'kind': 'upsert', 'collection': 'notes', 'id': 'sample-note',
                      'fields': {'title': 'Synthetic note'}, 'unset': [], 'base': {}, 'baseExists': False, 'baseRecord': {}}
                headers = {'Origin': origin, 'X-CSRF-Token': csrf(first)}
                altered = {'Origin': 'https://untrusted.invalid', 'X-CSRF-Token': csrf(first)}
                payload = {'clientId': 'fixture-device-one', 'operations': [op]}
                self.assertEqual(request(port, 'POST', a + 'api/sync', first, payload, altered)[0], 403)
                self.assertEqual(request(port, 'POST', a + 'api/sync', first, payload, {'Origin': origin})[0], 403)
                status, body, _ = request(port, 'POST', a + 'api/sync', first, payload, headers)
                self.assertEqual(status, 200, body)
                self.assertEqual(json_get(a + 'api/state', second)['state']['notes'][0]['title'], 'Synthetic note')
                self.assertFalse(json_get(b + 'api/state', other)['state'].get('notes'))
                self.assertEqual(len(json_get(a + 'api/state', second)['state']['notes']), 1)
                file_headers = {**headers, 'X-File-Name': 'sample.txt', 'X-File-Type': 'text/plain'}
                self.assertEqual(request(port, 'PUT', a + 'api/files/sample', first, b'fixture-text', file_headers)[0], 200)
                self.assertEqual(request(port, 'GET', a + 'api/files/sample', second, headers={'Range': 'bytes=0-6'})[:2], (206, b'fixture'))
                self.assertEqual(request(port, 'GET', b + 'api/files/sample', other)[0], 404)
                self.assertEqual(request(port, 'GET', a + 'api/files/sample', other)[0], 403)
                self.assertEqual(request(port, 'HEAD', a + 'api/files/sample', second)[2]['Content-Length'], '12')
                staged = a + 'api/files/staged?name=fixture.txt'
                chunk_one = {**headers, 'Content-Type': 'text/plain', 'Content-Range': 'bytes 0-3/8'}
                chunk_two = {**chunk_one, 'Content-Range': 'bytes 4-7/8'}
                self.assertEqual(request(port, 'PUT', staged, first, b'efgh', chunk_two)[0], 409)
                self.assertEqual(request(port, 'PUT', staged, first, b'abcd', chunk_one)[0], 200)
                self.assertEqual(request(port, 'GET', staged, second)[0], 404)
                self.assertEqual(request(port, 'PUT', staged, first, b'abcd', chunk_one)[0], 200)
                self.assertEqual(request(port, 'PUT', staged, first, b'efgh', chunk_two)[0], 200)
                self.assertEqual(request(port, 'PUT', staged, first, b'efgh', chunk_two)[0], 200)
                self.assertEqual(request(port, 'GET', staged, second)[:2], (200, b'abcdefgh'))
                gateway.sessions.pop(first)
                self.assertEqual(request(port, 'GET', a + 'api/state', first)[0], 401)


if __name__ == '__main__':
    if '--serve' in sys.argv:
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument('--serve', action='store_true')
        parser.add_argument('--work-dir', type=Path, required=True)
        parser.add_argument('--port', type=int, required=True)
        parser.add_argument('--tls-cert', type=Path)
        parser.add_argument('--tls-key', type=Path)
        options = parser.parse_args()
        tls = (options.tls_cert, options.tls_key) if options.tls_cert else None
        with scenario(options.work_dir, port=options.port, max_active=4, tls=tls) as (gateway, url):
            print(json.dumps({'url': url, 'tls': gateway.tls_origin}), flush=True)
            sys.stdin.read()
    else:
        unittest.main()
