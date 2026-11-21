"""Run probes independently from NEBULA and publish separate availability indicators."""
import argparse
import http.client
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import secrets
import time

from .scenarios import export, read_observer, span
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


def process_threads(pid):
    if not pid:
        return None
    try:
        for line in (Path("/proc") / str(pid) / "status").read_text().splitlines():
            if line.startswith("Threads:"):
                return int(line.split()[1])
    except OSError:
        pass
    return None


def serve(port, target, pid=None):
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
            threads = process_threads(pid)
            if threads is not None:
                signals["target_threads"] = threads
            body = "".join("# TYPE nebula_probe_" + k + " gauge\nnebula_probe_" +
                           k + " " + str(value) + "\n" for k, value in signals.items()).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; version=0.0.4")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    with HTTPServer(("127.0.0.1", port), ProbeHandler) as server:
        server.serve_forever()


def trace_request(target, otlp, cookie):
    trace_id, span_id = secrets.token_hex(16), secrets.token_hex(8)
    start_wall, start = time.time_ns(), time.monotonic_ns()
    request = {"traceparent": "00-" + trace_id + "-" + span_id + "-01", "Cookie": cookie}
    code, _ = probe_http(target, "/api/conversations", request)
    origin = span(trace_id, "probe GET /api/conversations", start_wall,
                  start_wall + time.monotonic_ns() - start, failed=code != 200, kind=3, span_id=span_id)
    export(otlp, "nebula-probe", [origin])
    return trace_id, code


def trace(target, otlp, check_observer=None):
    # Borrow the project's real test login and CSRF setup.
    from pathlib import Path
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts/project"))
    from repro_runtime import browser_fixture
    browser = browser_fixture(Path(__file__).resolve().parents[2])
    user = browser.BrowserClient("127.0.0.1", target)
    user.register_and_login("obs_" + secrets.token_hex(7), secrets.token_urlsafe(16))
    trace_id, code = trace_request(target, otlp, user.cookie)
    if code != 200:
        raise RuntimeError("Authenticated conversation probe did not return 200")
    result={"traceId":trace_id,"status":code,"path":"/?view=traces&trace="+trace_id}
    if check_observer:
        deadline=time.monotonic()+8
        while time.monotonic()<deadline:
            try:
                found=read_observer(check_observer,"/api/v1/traces/"+trace_id)
                names={row["name"] for row in found["spans"]}
                if found["status"]=="complete" and {"GET /api/conversations","PostgreSQL command"} <= names:
                    result["spanCount"]=len(found["spans"])
                    break
            except (OSError,ValueError):
                pass
            time.sleep(.2)
        else:
            result["warning"]="Observer 未在八秒内收到完整链路，请核对 OTLP 端口和业务导出状态"
    print(encode(result))


def running_lab(directory):
    path=directory/"session.json"
    deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        try:
            session=json.loads(path.read_text(encoding="utf-8"))
            if probe_http(session["nebulaPort"],"/health/live")[0]==200:
                return session
        except (OSError,ValueError):
            pass
        time.sleep(.25)
    raise RuntimeError("The lab business service did not become reachable")


def main():
    cli = argparse.ArgumentParser()
    cli.add_argument("command", choices=("serve", "trace"))
    cli.add_argument("--target-port", type=int)
    cli.add_argument("--run-dir",type=Path,help="Read the business and OTLP ports from an isolated lab")
    cli.add_argument("--port", default=18786, type=int)
    cli.add_argument("--target-pid", type=int)
    cli.add_argument("--otlp")
    args = cli.parse_args()
    check_observer=None
    if args.run_dir:
        session=running_lab(args.run_dir)
        args.target_port = args.target_port or session["nebulaPort"]
        ports = session["ports"]
        args.otlp = args.otlp or f"http://127.0.0.1:{ports.get('collector',ports['observer'])}"
        if session["profile"] in ('local','metrics'):
            check_observer=f"http://127.0.0.1:{ports['observer']}"
    if not args.target_port:
        cli.error("use --target-port or --run-dir")
    if args.command == "serve":
        serve(args.port, args.target_port, args.target_pid)
    else:
        trace(args.target_port, args.otlp or "http://127.0.0.1:14318",check_observer)


if __name__ == "__main__":
    main()
