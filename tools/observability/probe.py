"""Run probes independently from NEBULA and publish separate availability indicators."""
import argparse
import http.client
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import secrets
import time

from .scenarios import export, span
from .observer.store import encode


def probe_http(port, path, headers=None):
    client = http.client.HTTPConnection("127.0.0.1", port, timeout=1.5)
    try:
        client.request("GET", path, headers=headers or {})
        response = client.getresponse()
        return response.status, response.read(64 * 1024)
    except (OSError, http.client.HTTPException):
        return 0, b""
    finally:
        client.close()


def serve(port, target):
    class ProbeHandler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            self.send_error(405)

        def do_GET(self):
            if self.path == "/version":
                status, body = probe_http(target, "/health/version")
                self.send_response(200 if status == 200 else 503)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(body if status == 200 else b'{}')
                return
            if self.path != "/metrics":
                self.send_error(404)
                return
            signals = {key: int(probe_http(target, "/health/" + key)[0] == 200)
                       for key in ("live", "ready")}
            body = "".join("# TYPE nebula_probe_" + k + " gauge\nnebula_probe_" +
                           k + " " + str(value) + "\n" for k, value in signals.items()).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; version=0.0.4")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    with HTTPServer(("127.0.0.1", port), ProbeHandler) as server:
        server.serve_forever()


def trace(target, otlp):
    # Borrow the project's real test login and CSRF setup.
    from pathlib import Path
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts/project"))
    from repro_runtime import browser_fixture
    browser = browser_fixture(Path(__file__).resolve().parents[2])
    user = browser.BrowserClient("127.0.0.1", target)
    user.register_and_login("obs_" + secrets.token_hex(7), secrets.token_urlsafe(16))
    trace_id, span_id = secrets.token_hex(16), secrets.token_hex(8)
    start_wall, start = time.time_ns(), time.monotonic_ns()
    request = {"traceparent": "00-" + trace_id + "-" + span_id + "-01", "Cookie": user.cookie}
    code, _ = probe_http(target, "/api/conversations", request)
    origin = span(trace_id, "probe GET /api/conversations", start_wall,
                  start_wall + time.monotonic_ns() - start, failed=code != 200, kind=3, span_id=span_id)
    export(otlp, "nebula-probe", [origin])
    if code != 200:
        raise RuntimeError("Authenticated conversation probe did not return 200")
    print(encode({"traceId": trace_id, "status": code}))


def main():
    cli = argparse.ArgumentParser()
    cli.add_argument("command", choices=("serve", "trace"))
    cli.add_argument("--target-port", required=True, type=int)
    cli.add_argument("--port", default=18786, type=int)
    cli.add_argument("--otlp", default="http://127.0.0.1:14318")
    args = cli.parse_args()
    if args.command == "serve":
        serve(args.port, args.target_port)
    else:
        trace(args.target_port, args.otlp)


if __name__ == "__main__":
    main()
