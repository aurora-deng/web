"""Run a loopback Observer. Authentication belongs to the SSH access boundary."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from hmac import compare_digest
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import math
from pathlib import Path
import re
import sqlite3
import threading
import time
from urllib.parse import parse_qs, urlsplit

from .events import Invalid, hex_id, identifier, unsigned
from .projection import agent_cost, agent_summary, compare, device_summary, is_partial, trace_view
from .sources import BackendError, NoTrace, PANELS, Sources
from .store import Capacity, Conflict, DAY, Store, encode

ROOT = Path(__file__).resolve().parents[3]
DOMAINS = {"dag", "agent", "device", "service", "experiment"}
BODY_LIMIT = 256 * 1024
PATHS = {"/": ("index.html", "text/html; charset=utf-8"),
         "/app.js": ("app.js", "text/javascript; charset=utf-8"),
         "/style.css": ("style.css", "text/css; charset=utf-8")}


class ObserverServer(HTTPServer):
    def __init__(self, address, store, sources, credentials, echarts=None, pricing=None, preview=False):
        super().__init__(address, Handler)
        self.store, self.sources, self.credentials = store, sources, credentials
        self.echarts = echarts
        self.pricing = pricing
        self.preview = preview
        self.work = ThreadPoolExecutor(max_workers=8)
        self.slots = threading.BoundedSemaphore(16)
        self.last_prune = 0

    def get_request(self):
        conn, address = super().get_request()
        conn.settimeout(4)
        return conn, address

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        self.work.submit(self.handle_task, request, address)

    def handle_task(self, request, address):
        try:
            self.finish_request(request, address)
        except (OSError, TimeoutError):
            pass
        except Exception:
            self.handle_error(request, address)
        finally:
            self.shutdown_request(request)
            self.slots.release()

    def service_actions(self):
        if time.monotonic() - self.last_prune > 60:
            self.last_prune = time.monotonic()
            try:
                self.store.prune()
            except sqlite3.OperationalError:
                # A concurrent writer can still own the database; retry next tick.
                pass

    def server_close(self):
        super().server_close()
        self.work.shutdown(wait=True)


def read_credentials(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    credentials = []
    if not isinstance(data, dict) or len(data) > 16:
        raise ValueError("configure at most 16 authenticated producers")
    for source, row in data.items():
        if (not identifier(source) or row["domain"] not in DOMAINS or type(row["simulated"]) is not bool
                or not re.fullmatch(r"[A-Za-z0-9_-]{32,128}", row["token"])):
            raise ValueError("invalid telemetry producer")
        credentials.append((row["token"], source, row["domain"], row["simulated"]))
    if len({r[0] for r in credentials}) != len(credentials):
        raise ValueError("duplicate producer credentials")
    return credentials


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        # The request URL can contain trace identifiers. Keep service logs aggregate.
        pass

    def emit(self, status, payload, content_type="application/json; charset=utf-8"):
        body = payload if isinstance(payload, bytes) else encode(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; object-src 'none'; frame-ancestors 'none'")
        self.end_headers()
        self.wfile.write(body)

    def valid_host(self):
        supplied = self.headers.get("Host", "")
        host, sep, port = supplied.partition(":")
        return host in ("127.0.0.1", "localhost") and (not sep or (port.isdecimal() and 0 < int(port) <= 65535))

    def do_POST(self):
        if not self.valid_host() or self.headers.get("Origin"):
            self.emit(403, {"error": "此入口仅供本机受信生产者使用"})
            return
        if self.path != "/ingest/v1/events":
            self.emit(404, {"error": "接口不存在"})
            return
        scheme, _, token = self.headers.get("Authorization", "").partition(" ")
        identity = (next((r[1:] for r in self.server.credentials if compare_digest(r[0], token)), None)
                    if scheme.lower() == "bearer" and token.isascii() else None)
        if identity is None:
            self.emit(401, {"error": "上报凭据无效"})
            return
        try:
            length = self.headers.get("Content-Length", "")
            if self.headers.get("Transfer-Encoding") or not length.isdecimal() or len(length) > 8:
                raise Invalid("需要明确的请求体长度")
            if not 1 <= int(length) <= BODY_LIMIT:
                self.emit(413, {"error": "批次超过限制"})
                return
            if self.headers.get_content_type() != "application/json":
                self.emit(415, {"error": "需要 JSON"})
                return
            data = json.loads(self.rfile.read(int(length)))
            self.emit(200, self.server.store.append(*identity, data))
        except (Invalid, ValueError, RecursionError) as error:
            self.emit(400, {"error": str(error) if isinstance(error, Invalid) else "JSON 无效"})
        except Conflict as error:
            self.emit(409, {"error": str(error)})
        except Capacity as error:
            self.emit(503, {"error": str(error)})

    def do_GET(self):
        if not self.valid_host():
            self.emit(403, {"error": "仅接受本机 Host"})
            return
        try:
            request = urlsplit(self.path)
            if len(request.query) > 512:
                raise Invalid("查询参数过长")
            args = parse_qs(request.query, max_num_fields=10)
            if any(len(v) != 1 for v in args.values()):
                raise Invalid("查询参数重复")
            self.route(request.path, {k: v[0] for k, v in args.items()})
        except (Invalid, ValueError) as error:
            self.emit(400, {"error": str(error)})
        except BackendError as error:
            self.emit(503, {"status": "unavailable", "error": str(error)})

    def route(self, path, args):
        store, sources = self.server.store, self.server.sources
        if path in PATHS:
            name, content_type = PATHS[path]
            self.emit(200, (ROOT / "frontend/observability" / name).read_bytes(), content_type)
        elif path == "/echarts.js":
            # Keep the standard-library UI usable without emitting a failed script request.
            self.emit(200, self.server.echarts.read_bytes() if self.server.echarts else b"",
                      "text/javascript; charset=utf-8")
        elif path == "/health":
            self.emit(200, {"status": "alive", "simulated": self.server.preview})
        elif path == "/metrics":
            windows = store.latest_device_windows(time.time_ns() - 60 * 10**9)
            text = "# TYPE nebula_observer_device_window_samples gauge\n"
            text += "# TYPE nebula_observer_device_window_gaps gauge\n"
            for window in windows:
                fields = window["fields"]
                labels = '{source="%s",stream="%s"}' % (window["source"], fields["stream"])
                text += "nebula_observer_device_window_samples%s %d\n" % (labels, fields["samples"])
                text += "nebula_observer_device_window_gaps%s %d\n" % (labels, fields["gaps"])
            self.emit(200, text.encode(), "text/plain; version=0.0.4; charset=utf-8")
        elif path == "/api/v1/catalog":
            self.emit(200, [{"id": p, "title": v[0], "unit": v[1], "query": v[2], "guide": v[3]} for p, v in PANELS.items()])
        elif path == "/api/v1/overview":
            data = {}
            for key, expression in {"scrape": 'up{job="nebula"}', "alive": "nebula_probe_live",
                                     "ready": "nebula_probe_ready", "monitor": 'up{job="observer"}'}.items():
                vector = sources.prom("query", {"query": expression})["result"]
                data[key] = self.points([vector[0]["value"]])[0] if vector else None
            self.emit(200, {"status": "ok", "signals": data, "echarts": bool(self.server.echarts)})
        elif path == "/api/v1/version":
            version = sources.fetch(sources.probe, "/version")
            self.emit(200, {k: version.get(k) for k in ("revision", "sourceFingerprint", "requiredMigration")})
        elif path == "/api/v1/alerts":
            self.emit(200, sources.prom("alerts")["alerts"])
        elif path == "/api/v1/series":
            panel = args.get("panel")
            if panel not in PANELS:
                raise Invalid("未知面板")
            start, end = self.window(args)
            step = max(15, math.ceil((end - start) / 720 / 10**9))
            series = sources.prom("query_range", {"query": PANELS[panel][2], "start": start // 10**9,
                                                 "end": end // 10**9, "step": step})["result"]
            self.emit(200, {"status": "ok" if series else "empty", "step": step,
                            "series": [{"labels": x["metric"], "points": self.points(x["values"])} for x in series[:8]]})
        elif path == "/api/v1/events":
            start, end = self.window(args)
            domain = args.get("domain")
            if domain and domain not in DOMAINS:
                raise Invalid("未知事件类型")
            cursor = args.get("after")
            if cursor:
                pair = cursor.split(",")
                if len(pair) != 2 or not all(unsigned(i) and int(i) < 2**63 for i in pair):
                    raise Invalid("游标无效")
                cursor = tuple(map(int, pair))
            data = store.events(start, end, self.limit(args), cursor, domain=domain)
            if domain == "device":
                data["commands"] = device_summary(data["items"])
            self.emit(200, data)
        elif path == "/api/v1/markers":
            start, end = self.window(args)
            data = store.events(start, end, 100, domain="experiment")["items"]
            self.emit(200, sorted(data + store.run_markers(start, end), key=lambda x: int(x["occurredAt"])))
        elif path.startswith("/api/v1/traces/"):
            trace_id = path.rsplit("/", 1)[1]
            if not hex_id(trace_id, 32):
                raise Invalid("traceId 无效")
            now = time.time_ns()
            evidence = store.events(now - store.retention, now + 300 * 10**9, 256, trace=trace_id)
            state, spans = "complete", []
            try:
                raw = sources.trace(trace_id)
                spans = trace_view(raw)
                if is_partial(raw, spans):
                    state = "partial"
            except NoTrace:
                state = "partial" if evidence["items"] else "missing"
            except (BackendError, KeyError, TypeError):
                state = "unavailable"
            if (evidence["next"] or len(spans) > 1000) and state == "complete":
                state = "partial"
            self.emit(200 if state != "missing" else 404, {"traceId": trace_id, "status": state,
                      "spans": spans[:1000], "agent": agent_cost(agent_summary(evidence["items"]), self.server.pricing),
                      **evidence})
        elif path == "/api/v1/runs":
            start, end = self.window(args)
            self.emit(200, store.runs(start, end, self.limit(args)))
        elif path.startswith("/api/v1/runs/") and path != "/api/v1/runs/compare":
            digest = path.rsplit("/", 1)[1]
            if not re.fullmatch(r"[0-9a-f]{64}",digest):
                raise Invalid("实验编号无效")
            report = store.run(digest)
            self.emit(200 if report else 404, report or {"error":"实验记录不存在"})
        elif path == "/api/v1/runs/compare":
            def selected(name):
                ident = args.get(name, "")
                if not re.fullmatch(r"[0-9a-f]{64}", ident):
                    raise Invalid("实验编号无效")
                return store.run(ident)
            baseline, candidate = selected("baseline"), selected("candidate")
            if baseline is None or candidate is None:
                self.emit(404, {"error": "实验记录不存在"})
            else:
                self.emit(200, compare(baseline, candidate))
        else:
            self.emit(404, {"error": "接口不存在"})

    @staticmethod
    def points(values):
        result = []
        for stamp, raw in values:
            val = float(raw)
            result.append([stamp, val if math.isfinite(val) else None])
        return result

    @staticmethod
    def window(args):
        now = int(time.time())
        end = int(args.get("to", now))
        start = int(args.get("from", end - 900))
        if not 0 <= start < end <= now + 60 or end - start > 604800:
            raise Invalid("时间范围最大 7 天，使用 Unix 秒")
        return start * 10**9, end * 10**9

    @staticmethod
    def limit(args):
        return max(1, min(200, int(args.get("limit", 100))))


def main():
    cli = argparse.ArgumentParser()
    subs = cli.add_subparsers(dest="command", required=True)
    serve = subs.add_parser("serve")
    serve.add_argument("--db", type=Path, required=True)
    serve.add_argument("--credentials", type=Path, required=True)
    serve.add_argument("--echarts", type=Path)
    serve.add_argument("--port", type=int, default=18785)
    serve.add_argument("--prometheus", default="http://127.0.0.1:19090")
    serve.add_argument("--tempo", default="http://127.0.0.1:13200")
    serve.add_argument("--probe", default="http://127.0.0.1:18786")
    serve.add_argument("--agent-pricing", type=Path)
    imp = subs.add_parser("import-run")
    imp.add_argument("--db", type=Path, required=True)
    imp.add_argument("report", type=Path)
    mark = subs.add_parser("mark")
    mark.add_argument("--credentials", type=Path, required=True)
    mark.add_argument("--observer", default="http://127.0.0.1:18785")
    mark.add_argument("--run", required=True)
    mark.add_argument("--id", required=True)
    mark.add_argument("--scenario", required=True)
    mark.add_argument("--state", required=True, choices=("started","fault","recovered","finished","version"))
    args = cli.parse_args()
    if args.command == "mark":
        from tools.observability.scenarios import post
        source = json.loads(args.credentials.read_text(encoding="utf-8"))["experiment-lab"]["token"]
        event = {"schemaVersion":1,"eventId":args.id,"eventType":"experiment.marker",
                 "occurredAt":str(time.time_ns()),"fields":{"runId":args.run,"state":args.state,"scenario":args.scenario}}
        print(encode(post(args.observer,"/ingest/v1/events",[event],source)))
        return
    if args.command == "import-run":
        if args.report.stat().st_size > BODY_LIMIT:
            cli.error("report exceeds limit")
        print(Store(args.db).import_run(json.loads(args.report.read_text(encoding="utf-8"))))
        return
    if args.echarts and not args.echarts.is_file():
        cli.error("explicit ECharts file is missing")
    pricing = None
    if args.agent_pricing:
        pricing = json.loads(args.agent_pricing.read_text(encoding="utf-8"))
        if (not isinstance(pricing, dict) or set(pricing) != {"currency","inputTokens","outputTokens"}
                or not isinstance(pricing["currency"],str) or not re.fullmatch("[A-Z]{3}",pricing["currency"])
                or any(type(pricing[k]) not in (int,float) or not 0 <= pricing[k] < 1e6 for k in ("inputTokens","outputTokens"))):
            cli.error("Rates must be nonnegative per million tokens, with an ISO currency code")
    with ObserverServer(("127.0.0.1", args.port), Store(args.db),
                        Sources(args.prometheus, args.tempo, args.probe), read_credentials(args.credentials),
                        args.echarts, pricing) as http:
        try:
            http.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
