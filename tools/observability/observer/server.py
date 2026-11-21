"""Run a loopback Observer. Authentication belongs to the SSH access boundary."""
import argparse
from contextlib import ExitStack
from concurrent.futures import ThreadPoolExecutor
import gzip
from hmac import compare_digest
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import hashlib
import math
from pathlib import Path
import re
import secrets
import signal
import sqlite3
import threading
import time
from urllib.parse import parse_qs, urlsplit

from .events import Invalid, hex_id, identifier, unsigned
from .projection import agent_cost, agent_summary, compare, device_summary, is_partial, trace_view
from .sources import BackendError, NoTrace, PANELS, SIGNALS, Sources
from .store import Capacity, Conflict, DAY, Store, encode
from .source_monitor import SourceMonitor
from .workspace import Workspace, provision_files
from .target_http import read_auth
from .resource_history import ResourceHistory
from .custom_alerts import CustomAlerts
from .local_traces import FUNCTION_SPANS, ROOT_OPERATIONS
from .log_store import LogStore
from .structured_logging import DiagnosticLogger, LogTail,prune_log_files
from .operation_catalog import FUNCTION_DEFINITIONS, HTTP_OPERATIONS
from .trace_queries import TraceQueries, TraceSearchQuery
from .metric_queries import MetricQueries, points
from .experiment_queries import ExperimentQueries
from .access_preflight import metrics_access, private_file
from .gateway_logs import GatewayTelemetry, SOURCE as GATEWAY_LOG_SOURCE

ROOT = Path(__file__).resolve().parents[3]
DOMAINS = {"dag", "agent", "device", "service", "experiment", "function", "log"}
BODY_LIMIT = 256 * 1024
GZIP_ROUTES = frozenset({"/api/v1/series", "/api/v1/series/batch", "/api/v1/service-levels",
                         "/api/v1/resources", "/api/v1/workspace/query"})


def accepts_gzip(header):
    weights = {}
    for entry in header.lower().split(","):
        encoding, *parameters = entry.split(";")
        weight = 1.0
        for parameter in parameters:
            name, separator, value = parameter.strip().partition("=")
            if name == "q" and separator:
                try:
                    weight = float(value)
                except ValueError:
                    weight = 0.0
        weights[encoding.strip()] = weight
    return 0 < weights.get("gzip", weights.get("*", 0.0)) <= 1


def stop_requested(*_):
    raise KeyboardInterrupt


PATHS = {"/": ("index.html", "text/html; charset=utf-8"),
         "/app.js": ("app.js", "text/javascript; charset=utf-8"),
         "/workspace.js": ("workspace.js", "text/javascript; charset=utf-8"),
         "/explore.js": ("explore.js", "text/javascript; charset=utf-8"),
         "/query_editor.js": ("query_editor.js", "text/javascript; charset=utf-8"),
         "/logs.js": ("logs.js", "text/javascript; charset=utf-8"),
         "/custom_dashboards.js": ("custom_dashboards.js", "text/javascript; charset=utf-8"),
         "/dashboard_templates.js": ("dashboard_templates.js", "text/javascript; charset=utf-8"),
         "/distributed.js": ("distributed.js", "text/javascript; charset=utf-8"),
         "/trace_details.js": ("trace_details.js", "text/javascript; charset=utf-8"),
         "/function_traces.js": ("function_traces.js", "text/javascript; charset=utf-8"),
         "/alert_rules.js": ("alert_rules.js", "text/javascript; charset=utf-8"),
         "/reliability.js": ("reliability.js", "text/javascript; charset=utf-8"),
         "/function_performance.js": ("function_performance.js", "text/javascript; charset=utf-8"),
         "/resources.js": ("resources.js", "text/javascript; charset=utf-8"),
         "/commit_report.js": ("commit_report.js", "text/javascript; charset=utf-8"),
         "/dashboard.js": ("dashboard.js", "text/javascript; charset=utf-8"),
         "/dashboard_queries.js": ("dashboard_queries.js", "text/javascript; charset=utf-8"),
         "/series.js": ("series.js", "text/javascript; charset=utf-8"),
         "/chart.js": ("chart.js", "text/javascript; charset=utf-8"),
         "/chart_export.js": ("chart_export.js", "text/javascript; charset=utf-8"),
         "/style.css": ("style.css", "text/css; charset=utf-8")}


class ObserverServer(HTTPServer):
    def __init__(self, address, store, sources, credentials, echarts=None, pricing=None, preview=False, traces=None,
                 source_networks=(), source_token=None, source_auth=None, resource_targets=None,
                 log_files=(), related_observer=None, log_console=False,log_directory=None,log_input_directories=(),
                 gateway_access_log=None):
        if related_observer and (not re.fullmatch(r"http://127\.0\.0\.1:[0-9]{1,5}", related_observer)
                                 or not 0<int(related_observer.rsplit(":",1)[1])<65536):
            raise ValueError("Related Observer must be an explicitly configured loopback address")
        files = {route: ((ROOT / "frontend/observability" / name).read_bytes(), content_type)
                 for route, (name, content_type) in PATHS.items()}
        self.assets = files
        self.asset_version = hashlib.sha256(b"".join(
            path.encode() + hashlib.sha256(body).digest() for path, (body, _) in sorted(files.items())
        )).hexdigest()[:16]
        self.workspace = Workspace(store)
        super().__init__(address, Handler)
        self.store, self.sources, self.credentials = store, sources, credentials
        self.echarts = echarts
        self.pricing = pricing
        self.preview = preview
        self.traces = traces
        self.work = ThreadPoolExecutor(max_workers=8)
        self.slots = threading.BoundedSemaphore(16)
        self.last_prune = 0
        self.prune_pending = None
        self.target_monitor = SourceMonitor(self.workspace,sources,source_networks,self.server_port,source_token,source_auth)
        self.custom_alerts = CustomAlerts(self.workspace,self.target_monitor)
        self.resources = ResourceHistory(store,resource_targets or {})
        self.logs = LogStore(store)
        self.logger = DiagnosticLogger(self.logs,console=log_console,output_dir=log_directory)
        self.log_tail = LogTail(self.logs,log_files,log_input_directories) if log_files or log_input_directories else None
        self.gateway = (GatewayTelemetry(self.logs, gateway_access_log)
                        if gateway_access_log is not None else None)
        self.last_log_prune = None
        self.related_observer = related_observer
        self.last_resource = 0
        self.resource_pending = None
        self.resource_enabled = resource_targets is not None
        if not preview:
            self.target_monitor.start()
            self.custom_alerts.start()
        self.logger.emit("observer.started","INFO")

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
            self.logger.emit("observer.internal_error")
        finally:
            self.shutdown_request(request)
            self.slots.release()

    def service_actions(self):
        now = time.monotonic()
        if (self.resource_enabled and time.monotonic()-self.last_resource>=5
                and (not self.resource_pending or self.resource_pending.done())):
            self.last_resource=time.monotonic()
            self.resource_pending=self.work.submit(self.capture_resources)
        if now - self.last_prune > 60 and (not self.prune_pending or self.prune_pending.done()):
            self.last_prune = now
            self.prune_pending = self.work.submit(self.prune_history)

    def prune_history(self):
        try:
            self.store.prune()
            if self.logger.output_dir:prune_log_files(self.logger.output_dir,"observer")
            self.last_log_prune=int(time.time())
        except (sqlite3.OperationalError,OSError):
            # A concurrent writer can still own the database; retry next tick.
            pass

    def capture_resources(self):
        try:
            self.resources.capture()
        except (OSError,ValueError,sqlite3.OperationalError):
            # Missing proc entries and busy databases must never stop the web interface.
            self.logger.emit("observer.collection_failed","WARN")

    def server_close(self):
        if hasattr(self,"custom_alerts"):
            self.custom_alerts.close()
        if hasattr(self,"target_monitor"):
            self.target_monitor.close()
        super().server_close()
        self.work.shutdown(wait=True)
        if getattr(self,"log_tail",None):
            self.log_tail.close()
        if getattr(self, "gateway", None):
            self.gateway.close()
        if hasattr(self,"logger"):
            self.logger.emit("observer.stopped","INFO")
            self.logger.close()


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
    def send_error(self,code,message=None,explain=None):
        self.emit(code,{"error":"请求协议或方法不受支持"})

    def log_message(self, *_):
        # The request URL can contain trace identifiers. Keep service logs aggregate.
        pass

    def emit(self, status, payload, content_type="application/json; charset=utf-8",log_code=None):
        request_id = secrets.token_hex(8)
        if status>=400:
            code = log_code or ("observer.source_unavailable" if status==502 else "observer.store_unavailable" if status==503
                    else "observer.internal_error" if status>=500 else "observer.invalid_request")
            self.server.logger.emit(code,"ERROR" if status>=500 else "WARN",status=status,requestId=request_id)
        body = payload if isinstance(payload, bytes) else encode(payload).encode()
        compressed = False
        varies = status == 200 and len(body) >= 4096 and self.path.split("?", 1)[0] in GZIP_ROUTES
        if varies and accepts_gzip(self.headers.get("Accept-Encoding", "")):
            encoded = gzip.compress(body, compresslevel=1, mtime=0)
            if len(encoded) < len(body):
                body, compressed = encoded, True
        self.send_response(status)
        if varies:
            self.send_header("Vary", "Accept-Encoding")
        if compressed:
            self.send_header("Content-Encoding", "gzip")
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Observer-Request-ID",request_id)
        self.send_header("Content-Security-Policy", "default-src 'self'; img-src 'self' blob:; style-src 'self' 'unsafe-inline'; object-src 'none'; frame-ancestors 'none'")
        self.end_headers()
        self.wfile.write(body)

    def valid_host(self):
        supplied = self.headers.get("Host", "")
        host, sep, port = supplied.partition(":")
        return host in ("127.0.0.1", "localhost") and (not sep or (port.isdecimal() and 0 < int(port) <= 65535))

    def producer_identity(self):
        scheme, _, token = self.headers.get("Authorization", "").partition(" ")
        if scheme.lower() == "bearer" and token.isascii():
            return next((r[1:] for r in self.server.credentials if compare_digest(r[0], token)), None)
        return None

    def do_POST(self):
        if self.path.startswith("/api/v1/workspace/"):
            return self.workspace_post()
        if not self.valid_host() or self.headers.get("Origin"):
            self.emit(403, {"error": "此入口仅供本机受信生产者使用"})
            return
        if self.path == "/v1/traces" and self.server.traces:
            return self.receive_traces()
        if self.path == "/ingest/v1/function-traces" and self.server.traces:
            identity = self.producer_identity()
            if identity is None:
                return self.emit(401, {"error":"上报凭据无效"})
            if identity[1] != "function" or identity[2]:
                return self.emit(403, {"error":"该来源没有真实函数链路权限"})
            return self.receive_traces(identity[0])
        if self.path not in ("/ingest/v1/events", "/ingest/v1/logs"):
            self.emit(404, {"error": "接口不存在"})
            return
        identity = self.producer_identity()
        if identity is None:
            self.emit(401, {"error": "上报凭据无效"})
            return
        if (self.path=="/ingest/v1/logs") != (identity[1]=="log"):
            return self.emit(403,{"error":"该凭据的日志与事件权限不匹配"})
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
            result = (self.server.logs.append(identity[0],data) if self.path=="/ingest/v1/logs"
                      else self.server.store.append(*identity,data))
            self.emit(200, result)
        except (Invalid, ValueError, RecursionError) as error:
            self.emit(400, {"error": str(error) if isinstance(error, Invalid) else "JSON 无效"})
        except Conflict as error:
            self.emit(409, {"error": str(error)})
        except Capacity as error:
            self.emit(503, {"error": str(error)})
        except sqlite3.OperationalError:
            self.emit(503,{"error":"日志或事件存储暂不可写"})
        except Exception:
            self.emit(500,{"error":"上报处理失败，请凭响应头的请求编号查看日志"})

    def workspace_post(self):
        origin = self.headers.get("Origin")
        if (not self.valid_host() or self.headers.get("X-Observer-Workspace") != "1"
                or origin not in (None, "http://"+self.headers.get("Host",""))):
            return self.emit(403, {"error":"工作区只接受本机同源请求"})
        try:
            length = self.headers.get("Content-Length", "")
            if self.headers.get("Transfer-Encoding") or len(length)>8 or not length.isdecimal():
                raise Invalid("需要明确的请求体长度")
            if not 1 <= int(length) <= BODY_LIMIT:
                return self.emit(413, {"error":"请求体超出限制"})
            if self.headers.get_content_type()!="application/json":
                return self.emit(415, {"error":"需要 JSON"})
            req=json.loads(self.rfile.read(int(length)))
            if not isinstance(req,dict):
                raise Invalid("配置需要 JSON 对象")
            op=self.path.removeprefix("/api/v1/workspace/")
            workspace, monitor=self.server.workspace,self.server.target_monitor
            if self.server.preview and (op.startswith("alert/") or op in ("source/test","source/save")
                                        or op=="query" and req.get("source")!="nebula"):
                raise Invalid("演示模式请使用内置数据，连接服务请打开实时入口")
            if op in ("query","labels","investigate","profile"):
                start,end=self.window(req)
                if op=="investigate" and req.get("source")=="nebula":
                    trace=req.get("trace","")
                    if not self.server.traces:
                        raise Invalid("当前档没有独立链路索引，请连接 Tempo")
                    if trace and not hex_id(trace,32):
                        raise Invalid("Trace ID 无效")
                    if trace:
                        try:
                            data=self.server.traces.raw(trace)
                        except NoTrace:
                            return self.emit(404,{"error":"内置索引未收到这条链路"})
                    else:
                        rows=self.server.traces.search(start,end,"",req.get("failed") is True,30)
                        data={"traces":[{"traceID":r["traceId"],"rootServiceName":r["service"],
                                         "rootTraceName":r["name"],"durationMs":r["durationMs"]} for r in rows]}
                else:
                    if self.server.preview and op!="query":
                        raise Invalid("此功能需要实时数据源")
                    action={"query":monitor.query,"labels":monitor.labels,"investigate":monitor.investigate,"profile":monitor.profile}[op]
                    data=action(req,start//10**9,end//10**9)
            elif op=="source/test":
                data=monitor.check(req)
            elif op=="alert/preview":
                data=self.server.custom_alerts.preview(workspace.alert(req))
            elif op in ("source/save","board/save","alert/save"):
                if op=="source/save":
                    if req.get("type")=="metrics" and not req.get("metrics"):
                        raise Invalid("文本采集至少需配置一个指标")
                    result=monitor.check(req)
                    if result.get("missing"):
                        raise Invalid("目标暂未返回以下指标："+", ".join(result["missing"]))
                data=workspace.save(op.split("/")[0],req)
            elif op in ("source/delete","board/delete","alert/delete"):
                workspace.remove(op.split("/")[0],req)
                data={"deleted":True}
            else:
                return self.emit(404, {"error":"工作区接口不存在"})
            self.emit(200,data)
        except (Invalid,ValueError,RecursionError) as error:
            self.emit(400, {"error":str(error) if isinstance(error,Invalid) else "工作区参数无效"})
        except Conflict as error:
            self.emit(409, {"error":str(error)})
        except BackendError as error:
            self.emit(502, {"error":str(error)})
        except sqlite3.OperationalError:
            self.emit(503, {"error":"配置存储暂不可用"})
        except Exception:
            self.emit(500,{"error":"工作区处理失败，请凭响应头的请求编号查看日志"})

    def receive_traces(self, producer=None):
        length = self.headers.get("Content-Length", "")
        if self.headers.get("Transfer-Encoding") or not length.isdecimal() or len(length) > 8:
            return self.emit(400, {"error": "需要明确的请求体长度"})
        if not 1 <= int(length) <= BODY_LIMIT:
            return self.emit(413, {"error": "链路批次超过限制"})
        if self.headers.get_content_type() != "application/json":
            return self.emit(415, {"error": "轻量档只接收本机 OTLP JSON"})
        try:
            payload=json.loads(self.rfile.read(int(length)))
            self.server.traces.append(payload, producer)
            try:
                self.server.logs.from_spans(payload)
            except (Invalid,Conflict,sqlite3.Error):
                self.server.logger.emit("observer.collection_failed","WARN")
            self.emit(200, {})
        except (Invalid, ValueError, RecursionError) as error:
            self.emit(400, {"error": str(error) if isinstance(error, Invalid) else "JSON 无效"})
        except sqlite3.OperationalError:
            self.emit(503, {"error": "链路存储暂不可写"})

    def do_GET(self):
        if not self.valid_host():
            self.emit(403, {"error": "仅接受本机 Host"})
            return
        try:
            request = urlsplit(self.path)
            if len(request.query) > (2048 if request.path in ("/api/v1/series/batch","/api/v1/logs") else 512):
                raise Invalid("查询参数过长")
            args = parse_qs(request.query, max_num_fields=16)
            if any(len(v) != 1 for v in args.values()):
                raise Invalid("查询参数重复")
            self.route(request.path, {k: v[0] for k, v in args.items()})
        except (Invalid, ValueError) as error:
            self.emit(400, {"error": str(error)})
        except BackendError as error:
            self.emit(503, {"status": "unavailable", "error": str(error)},log_code="observer.source_unavailable")
        except sqlite3.OperationalError:
            self.emit(503, {"status": "unavailable", "error": "监控存储暂不可读，请重试"})
        except Exception:
            self.emit(500,{"status":"unavailable","error":"监控处理失败，请凭请求编号查看日志"})

    def route(self, path, args):
        store, sources = self.server.store, self.server.sources
        if path == "/api/v1/logs":
            start,end=self.window(args)
            self.emit(200,{**self.server.logs.search(start,end,args),"pipeline":self.server.logger.status(),
                           "fileRejected":self.server.log_tail.rejected if self.server.log_tail else 0,
                           "lastCleanupAt":self.server.last_log_prune,"cleanupIntervalSeconds":60,
                           "retentionDays":store.retention_seconds//86400})
        elif path == "/api/v1/function-catalog":
            self.emit(200,{"functions":FUNCTION_DEFINITIONS,"httpOperations":sorted(HTTP_OPERATIONS),"fixedCardinality":True})
        elif path == "/api/v1/instrumentation":
            start,end=self.window(args)
            data=sources.instrumentation(start//10**9,end//10**9) if hasattr(sources,"instrumentation") else {
                "status":"external","guide":"此档通过 Prometheus 查询；请核对目标的 nebula_function_seconds 指标族。"}
            self.emit(200,{**data,"relatedObserver":self.server.related_observer})
        elif path in self.server.assets:
            body, content_type = self.server.assets[path]
            self.emit(200, body, content_type)
        elif path == "/favicon.ico":
            self.emit(204,b"","image/x-icon")
        elif path == "/echarts.js":
            # Keep the standard-library UI usable without emitting a failed script request.
            self.emit(200, self.server.echarts.read_bytes() if self.server.echarts else b"",
                      "text/javascript; charset=utf-8")
        elif path == "/health":
            source = "preview" if self.server.preview else getattr(sources, "profile", "prometheus")
            self.emit(200, {"status": "alive", "simulated": self.server.preview, "source": source,
                            "assetVersion": self.server.asset_version, "seriesBatch": hasattr(sources,"ranges"),
                            "localScope":getattr(sources,"scope_name",None),
                            "traceSearch": bool(self.server.traces) or source=="prometheus"})
        elif path == "/api/v1/scopes":
            if hasattr(sources,"scope"):
                items=[sources.scope()]
            elif self.server.preview:
                items=[]
            else:
                rows=sources.prom("query",{"query":'up{job="nebula"}'}).get("result",[])
                items=[{key:row["metric"].get(key,"") for key in ("bc","app","instance")}
                       for row in rows[:100] if row.get("metric",{}).get("instance")]
            self.emit(200,{"items":items})
        elif path == "/metrics":
            windows = store.latest_device_windows(time.time_ns() - 60 * 10**9)
            text = "# TYPE nebula_observer_device_window_samples gauge\n"
            text += "# TYPE nebula_observer_device_window_gaps gauge\n"
            for window in windows:
                fields = window["fields"]
                labels = '{source="%s",stream="%s"}' % (window["source"], fields["stream"])
                text += "nebula_observer_device_window_samples%s %d\n" % (labels, fields["samples"])
                text += "nebula_observer_device_window_gaps%s %d\n" % (labels, fields["gaps"])
            if self.server.gateway:
                text += self.server.gateway.metrics()
            self.emit(200, text.encode(), "text/plain; version=0.0.4; charset=utf-8")
        elif path == "/api/v1/catalog":
            self.emit(200, [{"id": p, "title": v[0], "unit": v[1], "query": v[2], "guide": v[3]} for p, v in PANELS.items()])
        elif path == "/api/v1/workspace":
            self.emit(200, {"boards":self.server.workspace.list("board"),"simulated":self.server.preview,
                           "internalTrace":bool(self.server.traces),
                           "gatewayLogSource": GATEWAY_LOG_SOURCE if self.server.gateway else None,
                           "suggestedMetricsUrl":(self.server.target_monitor.token or (None,None))[0],
                           "sources":[{**src,"lastCheck":self.server.target_monitor.current(src["id"])}
                                      for src in self.server.workspace.list("source")]})
        elif path == "/api/v1/resources":
            start,end=self.window(args)
            data=self.server.resources.history(start//10**9,end//10**9)
            self.emit(200,{**data,"collecting":self.server.resource_enabled})
        elif path == "/api/v1/resources/reports":
            self.emit(200,self.server.resources.reports())
        elif path.startswith("/api/v1/resources/reports/"):
            data=self.server.resources.reports(path.rsplit("/",1)[1])
            self.emit(200 if data else 404,data if data else {"error":"资源报告不存在"})
        elif path == "/api/v1/overview":
            data = {}
            for key, expression in SIGNALS.items():
                try:
                    vector = sources.prom("query", {"query": expression})["result"]
                    data[key] = self.points([vector[0]["value"]])[0] if vector else None
                except BackendError:
                    data[key] = None
            status = "ok" if all(point is not None for point in data.values()) else "partial"
            self.emit(200, {"status": status, "signals": data, "echarts": bool(self.server.echarts)})
        elif path == "/api/v1/version":
            version = sources.fetch(sources.probe, "/version")
            self.emit(200, {k: version.get(k) for k in ("revision", "sourceFingerprint", "requiredMigration")})
        elif path == "/api/v1/alerts":
            rows=list(sources.prom("alerts")["alerts"])
            for rule in self.server.custom_alerts.view():
                result=rule["result"]
                if result["state"] in ("pending","firing"):
                    rows.append({"labels":{"alertname":rule["id"]},"state":result["state"],
                                 "activeAt":time.strftime("%Y-%m-%dT%H:%M:%SZ",time.gmtime(result["activeSince"])),
                                 "value":result["value"],"annotations":{"title":rule["name"],"guide":rule["impact"],
                                    "unit":rule["unit"],"window":str(rule["windowSeconds"])+"s",
                                    "expression":rule["expr"],"custom":True}})
            self.emit(200, rows)
        elif path == "/api/v1/alert-rules":
            self.emit(200,{"rules":self.server.custom_alerts.view(),"history":self.server.custom_alerts.history()})
        elif path == "/api/v1/service-levels":
            start, end = (ns//10**9 for ns in self.window(args))
            if self.server.preview or not hasattr(sources, "service_levels"):
                self.emit(200, {"source": "preview" if self.server.preview else "prometheus",
                                "status": "unsupported", "from": start, "to": end})
            elif isinstance(sources, Sources):
                instance = args.get("instance", "")
                if len(instance) > 128:
                    raise Invalid("实例标识过长")
                self.emit(200, sources.service_levels(start, end, instance))
            else:
                self.emit(200, sources.service_levels(start, end))
        elif path == "/api/v1/series":
            start,end=self.window(args)
            self.emit(200,MetricQueries(sources).panel(args.get('panel'),start,end))
        elif path == "/api/v1/series/batch":
            start,end=self.window(args)
            self.emit(200,MetricQueries(sources).batch(args.get('panels','').split(','),start,end))
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
            producer = args.get("source")
            if producer is not None and not identifier(producer):
                raise Invalid("来源名称无效")
            data = store.events(start, end, self.limit(args), cursor, domain=domain, source=producer)
            if domain == "device":
                data["commands"] = device_summary(data["items"])
            self.emit(200, data)
        elif path == "/api/v1/markers":
            start, end = self.window(args)
            data = store.events(start, end, 100, domain="experiment")["items"]
            self.emit(200, sorted(data + store.run_markers(start, end), key=lambda x: int(x["occurredAt"])))
        elif path == "/api/v1/traces":
            start,end=self.window(args)
            query=TraceSearchQuery(start,end,self.limit(args),args.get('operation',''),args.get('function',''),
                args.get('statusClass',''),args.get('failed')=='1',args.get('q',''),
                float(args.get('minMs','0')),args.get('sort','recent'))
            self.emit(200,TraceQueries(store,sources,self.server.traces,self.server.pricing).search(query))
        elif path.startswith('/api/v1/traces/'):
            status,data=TraceQueries(store,sources,self.server.traces,self.server.pricing).detail(path.rsplit('/',1)[1])
            self.emit(status,data)
        elif path == '/api/v1/runs':
            start,end=self.window(args)
            self.emit(200,store.runs(start,end,self.limit(args)))
        elif path == '/api/v1/runs/compare':
            self.emit(*ExperimentQueries(store).compare(args.get('baseline',''),args.get('candidate','')))
        elif path.startswith('/api/v1/runs/'):
            report=ExperimentQueries(store).detail(path.rsplit('/',1)[1])
            self.emit(200 if report else 404,report or {'error':'实验记录不存在'})
        else:
            self.emit(404, {"error": "接口不存在"})

    points=staticmethod(points)

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
    serve.add_argument("--local-target", type=int)
    serve.add_argument("--local-target-pid", type=int)
    serve.add_argument("--local-scope", choices=("lab","daily"), default="lab")
    serve.add_argument("--scrape-token-file", type=Path)
    serve.add_argument("--require-ops",action="store_true",help="Reject startup unless anonymous metrics fail and ops metrics succeed")
    serve.add_argument("--member-token-file",type=Path,help="Also require a non-ops identity to receive 403")
    serve.add_argument("--metrics-prometheus",help="Use an installed Prometheus for panels while keeping local OTLP ingestion")
    serve.add_argument("--local-otlp", action="store_true")
    serve.add_argument("--source-network",action="append",default=[],help="Explicit CIDR allowed for browser-configured monitoring targets")
    serve.add_argument("--source-auth-file",type=Path)
    serve.add_argument("--related-observer",help="Explicit related loopback Observer with instrumented business data")
    serve.add_argument("--log-file",action="append",default=[],metavar="SOURCE=PATH",help="Tail only NEBULA_LOG JSON records")
    serve.add_argument("--log-input-directory",action="append",default=[],metavar="SOURCE=PATH")
    serve.add_argument("--gateway-access-log", type=Path)
    serve.add_argument("--log-directory",type=Path,help="Owned seven-day rotated Observer log directory")
    serve.add_argument("--log-console",action=argparse.BooleanOptionalAction,default=False)
    serve.add_argument("--resource-target",action="append",default=[], metavar="SERVICE=PID")
    serve.add_argument("--dashboards-dir",type=Path,help="Provision dashboard JSON on startup")
    provisioning=subs.add_parser("provision")
    provisioning.add_argument("--db",type=Path,required=True)
    provisioning.add_argument("--directory",type=Path,required=True)
    provisioning.add_argument("--check",action="store_true")
    resource= subs.add_parser("import-resource")
    resource.add_argument("--db",type=Path,required=True)
    resource.add_argument("--ack-profile",type=Path,help="Optional paired ACK profile from the same binary")
    resource.add_argument("report",type=Path)
    commit_report=subs.add_parser("import-commit",help="Archive an isolated PostgreSQL COMMIT experiment")
    commit_report.add_argument("--db",type=Path,required=True)
    commit_report.add_argument("--id",required=True)
    commit_report.add_argument("--label",required=True)
    commit_report.add_argument("report",type=Path)
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
    if args.command=="provision":
        print(encode(provision_files(args.directory,Workspace(Store(args.db)),args.check)))
        return
    if args.command == "mark":
        from tools.observability.scenarios import post
        source = json.loads(args.credentials.read_text(encoding="utf-8"))["experiment-lab"]["token"]
        event = {"schemaVersion":1,"eventId":args.id,"eventType":"experiment.marker",
                 "occurredAt":str(time.time_ns()),"fields":{"runId":args.run,"state":args.state,"scenario":args.scenario}}
        print(encode(post(args.observer,"/ingest/v1/events",[event],source)))
        return
    if args.command == "import-resource":
        print(ResourceHistory(Store(args.db),{}).import_file(args.report,args.ack_profile))
        return
    if args.command == "import-commit":
        try:
            print(ResourceHistory(Store(args.db),{}).import_commit(args.report,args.id,args.label))
        except (Invalid,OSError) as error:
            cli.error(str(error))
        return
    if args.command == "import-run":
        if args.report.stat().st_size > BODY_LIMIT:
            cli.error("report exceeds limit")
        print(Store(args.db).import_run(json.loads(args.report.read_text(encoding="utf-8"))))
        return
    if args.echarts and not args.echarts.is_file():
        cli.error("explicit ECharts file is missing")
    for credential_path in (args.credentials,args.scrape_token_file,args.member_token_file,args.source_auth_file):
        if credential_path:
            private_file(credential_path)
    if args.require_ops and (not args.local_target or not args.scrape_token_file):
        cli.error('--require-ops needs a local target and a private scrape token file')
    if args.metrics_prometheus and not args.local_target:
        cli.error('--metrics-prometheus requires a local target for the access check')
    if args.gateway_access_log and not args.gateway_access_log.is_absolute():
        cli.error("Gateway collection requires an absolute log path")
    pricing = None
    if args.agent_pricing:
        pricing = json.loads(args.agent_pricing.read_text(encoding="utf-8"))
        if (not isinstance(pricing, dict) or set(pricing) != {"currency","inputTokens","outputTokens"}
                or not isinstance(pricing["currency"],str) or not re.fullmatch("[A-Z]{3}",pricing["currency"])
                or any(type(pricing[k]) not in (int,float) or not 0 <= pricing[k] < 1e6 for k in ("inputTokens","outputTokens"))):
            cli.error("Rates must be nonnegative per million tokens, with an ISO currency code")
    with ExitStack() as stack:
        store = Store(args.db)
        log_files=[]
        for item in args.log_file:
            source,sep,path=item.partition("=")
            if not sep or not identifier(source) or not Path(path).is_absolute() or len(log_files)>=8:
                cli.error("Log file must be SOURCE=ABSOLUTE_PATH. At most eight files.")
            log_files.append((source,path))
        log_directories=[]
        for item in args.log_input_directory:
            source,sep,path=item.partition("=")
            if not sep or not identifier(source) or not Path(path).is_absolute() or len(log_directories)>=8:
                cli.error("Log directory must be SOURCE=ABSOLUTE_PATH. At most eight directories.")
            log_directories.append((source,path))
        if args.dashboards_dir:
            print(encode({"dashboards":provision_files(args.dashboards_dir,Workspace(store))}),flush=True)
        traces = None
        source_token = None
        resource_targets={}
        for item in args.resource_target:
            name,sep,pid=item.partition("=")
            if not sep or not identifier(name) or not pid.isdecimal() or not 0<int(pid)<2**31 or len(resource_targets)>=16:
                cli.error("Resource target must be SERVICE=PID. At most 16 targets.")
            resource_targets[name]=int(pid)
        if args.local_target_pid:
            resource_targets.setdefault("NEBULA",args.local_target_pid)
        if args.local_target:
            if not 0 < args.local_target < 65536 or (args.local_scope=="lab" and not args.scrape_token_file):
                cli.error("Local sampling needs a loopback target port; isolated labs need a scrape token")
            from .local_metrics import LocalMetrics
            from ..probe import process_threads
            token = args.scrape_token_file.read_text().strip() if args.scrape_token_file else ""
            if args.scrape_token_file and not token:
                cli.error("The scrape token file is empty")
            if args.require_ops:
                print(encode({'metricsAccess':metrics_access(args.local_target,token,
                    args.member_token_file.read_text().strip() if args.member_token_file else None)}),flush=True)
            if token:
                source_token = ("http://127.0.0.1:"+str(args.local_target)+"/metrics", token)
            sources = stack.enter_context(LocalMetrics(args.local_target, token,
                                                       args.local_target_pid, process_threads, store,args.local_scope))
            if args.metrics_prometheus:
                sources = Sources(args.metrics_prometheus,args.tempo,args.probe)
            if args.local_otlp:
                from .local_traces import TraceIndex
                traces = TraceIndex(store)
        else:
            if args.local_otlp:
                cli.error("Local OTLP requires a local scrape target")
            sources = Sources(args.prometheus, args.tempo, args.probe)
        http = stack.enter_context(ObserverServer(("127.0.0.1", args.port), store, sources,
                                                   read_credentials(args.credentials), args.echarts, pricing, traces=traces,
                                                   source_networks=args.source_network, source_token=source_token,
                                                   source_auth=read_auth(args.source_auth_file) if args.source_auth_file else None,
                                                   related_observer=args.related_observer,log_files=log_files,log_console=args.log_console,
                                                   log_directory=args.log_directory or args.db.parent/"structured-logs",
                                                   log_input_directories=log_directories,
                                                   gateway_access_log=args.gateway_access_log,
                                                   resource_targets=resource_targets if resource_targets or args.local_target else None))
        try:
            signal.signal(signal.SIGTERM,stop_requested)
            http.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
