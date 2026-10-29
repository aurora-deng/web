"""Controlled TLS peer on the approved isolated port. No real account/DB writes.

Run only while run_server.py is stopped. Reuses the approved learning certificate;
never changes a trust store, daily port, certificate or environment installation.
"""
import http.server
import socket
import ssl
import threading
import time
from pathlib import Path

assert Path('/home/pikachu/function3-src').is_dir()
certificate=Path('/home/pikachu/phase11-data/function3/runtime/localhost.crt')
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    writes=0
    lock=threading.Lock()
    def log_message(self,*args):pass
    def do_POST(self):self.do_GET()
    def do_GET(self):
        length=int(self.headers.get('Content-Length','0'))
        if not 0<=length<=1024:self.close_connection=True;return
        self.rfile.read(length)
        if self.path=='/ambiguous':
            with self.lock:Handler.writes+=1
            self.close_connection=True;return
        if self.path=='/hold':time.sleep(.5)
        value=str(Handler.writes).encode() if self.path=='/writes' else b'x'*(2*1024*1024+1) if self.path=='/large' else b'ok'
        self.send_response(200);self.send_header('Content-Length',str(len(value)))
        if self.path=='/close':self.send_header('Connection','close')
        self.end_headers()
        try:self.wfile.write(value);self.wfile.flush()
        except (ssl.SSLError,ConnectionError):pass
        if self.path in ('/close','/stale'):self.close_connection=True

server=http.server.ThreadingHTTPServer(('127.0.0.1',18444),Handler)
server.daemon_threads=True
context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.minimum_version=ssl.TLSVersion.TLSv1_3
context.load_cert_chain(certificate,certificate.with_suffix('.key'));context.set_alpn_protocols(['http/1.1'])
server.socket=context.wrap_socket(server.socket,server_side=True)
print('Owned synthetic TLS fault peer listening on isolated 18444',flush=True)
try:server.serve_forever()
except KeyboardInterrupt:pass
finally:server.server_close()
