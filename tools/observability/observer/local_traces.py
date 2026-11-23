"""Small, private OTLP/JSON trace index for a single NEBULA development lab."""
import time

from .events import Invalid, hex_id, identifier, unsigned
from .sources import FUNCTIONS, NoTrace
from .operation_catalog import HTTP_OPERATIONS
from .store import WEEK_SECONDS

TTL_NS = WEEK_SECONDS * 10**9
MAX_SPANS = 24000
MAX_BATCH = 256
FUNCTION_SPANS = frozenset(FUNCTIONS.values())
ROOT_OPERATIONS = HTTP_OPERATIONS | {'WS chat.send','ResilientModelProvider::generation',
                                    'RankingService::settle','SpeechService::runOne'}
OPERATIONS = {
    "nebula-webserver": ROOT_OPERATIONS | FUNCTION_SPANS |
        {"PostgresConversationRepository::listConversationSummaries", "PostgreSQL command"},
    "nebula-probe": {"probe GET /api/conversations", "probe GET /api/tree/posts"},
}
SPAN_KINDS = {"SPAN_KIND_" + name: code for code, name in enumerate(
    ("UNSPECIFIED", "INTERNAL", "SERVER", "CLIENT", "PRODUCER", "CONSUMER"))}


def allowed_spans(payload, now, producer=None):
    if not isinstance(payload, dict) or not isinstance(payload.get("resourceSpans"), list):
        raise Invalid("需要 OTLP JSON resourceSpans")
    rows = []
    for resource in payload["resourceSpans"]:
        attributes = resource.get("resource", {}).get("attributes", [])
        labels = {a.get("key"): a.get("value", {}).get("stringValue") for a in attributes}
        service = labels.get("service.name")
        if (producer and service != producer) or (not producer and service not in OPERATIONS):
            raise Invalid("未知链路来源")
        instance = labels.get("service.instance.id")
        environment = labels.get("deployment.environment.name")
        instance = instance if identifier(instance) and len(instance) <= 64 else None
        environment = environment if identifier(environment) and len(environment) <= 64 else None
        for scope in resource.get("scopeSpans", []):
            for entry in scope.get("spans", []):
                trace, span = entry.get("traceId"), entry.get("spanId")
                parent = entry.get("parentSpanId", "")
                start, end = entry.get("startTimeUnixNano"), entry.get("endTimeUnixNano")
                name, status = entry.get("name"), entry.get("status", {}).get("code", 0)
                kind = entry.get("kind")
                if isinstance(kind, str):
                    kind = SPAN_KINDS.get(kind)
                if type(kind) is not int or not 1 <= kind <= 5:
                    kind = None
                if (not hex_id(trace, 32) or not hex_id(span, 16)
                        or (parent and not hex_id(parent, 16))
                        or not unsigned(start) or not unsigned(end) or not now-TTL_NS <= int(start) <= int(end) <= now+300*10**9
                        or not (identifier(name) if producer else isinstance(name,str) and name in OPERATIONS[service])
                        or type(status) is not int or status not in (0, 1, 2)):
                    raise Invalid("链路字段或时间超出接入约束")
                # Store only fixed fields. Body, query values and arbitrary attributes stay out.
                attributes = entry.get("attributes", [])
                cpu = None
                http_status = None
                if producer:
                    cpu = next((a.get("value",{}).get("intValue") for a in attributes
                                if a.get("key")=="nebula.thread_cpu_ns"),None)
                    if cpu is not None and (not unsigned(cpu) or int(cpu)>int(end)-int(start)+1_000_000):
                        raise Invalid("函数线程 CPU 耗时无效")
                elif name in HTTP_OPERATIONS:
                    code = str(next((a.get("value", {}).get("intValue") for a in attributes
                                     if a.get("key") == "http.response.status_code"), ""))
                    if len(code) == 3 and code.isascii() and code.isdecimal() and 100 <= int(code) <= 599:
                        http_status = int(code)
                rows.append((trace, span, int(start), int(end), service, name, parent, status,
                             int(cpu) if cpu is not None else None, kind, http_status, instance, environment))
                if len(rows) > MAX_BATCH:
                    raise Invalid("单批 span 超过限制")
    if not rows:
        raise Invalid("单批至少需要一个 span")
    return rows


class TraceIndex:
    def __init__(self, store):
        self.store = store
        with store.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS trace_spans(
                    trace TEXT NOT NULL, span TEXT NOT NULL, started INTEGER NOT NULL,
                    ended INTEGER NOT NULL, service TEXT NOT NULL, name TEXT NOT NULL,
                    parent TEXT NOT NULL, status INTEGER NOT NULL,
                    PRIMARY KEY(trace,span));
                CREATE INDEX IF NOT EXISTS trace_start ON trace_spans(started);
            """)
            db.execute("BEGIN IMMEDIATE")
            columns = {row["name"] for row in db.execute("PRAGMA table_info(trace_spans)")}
            if "thread_cpu_ns" not in columns:
                db.execute("ALTER TABLE trace_spans ADD COLUMN thread_cpu_ns INTEGER")
            if "kind" not in columns:
                db.execute("ALTER TABLE trace_spans ADD COLUMN kind INTEGER")
            if "http_status" not in columns:
                db.execute("ALTER TABLE trace_spans ADD COLUMN http_status INTEGER")
            if "instance" not in columns:
                db.execute("ALTER TABLE trace_spans ADD COLUMN instance TEXT")
            if "environment" not in columns:
                db.execute("ALTER TABLE trace_spans ADD COLUMN environment TEXT")

    def append(self, payload, producer=None):
        try:
            rows = allowed_spans(payload, time.time_ns(), producer)
        except (AttributeError, KeyError, TypeError) as error:
            raise Invalid("链路请求格式无效") from error
        with self.store.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            db.executemany("INSERT OR IGNORE INTO trace_spans(trace,span,started,ended,service,name,parent,status,thread_cpu_ns,kind,http_status,instance,environment)"
                           " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)", rows)
            size = db.execute("SELECT count(*) FROM trace_spans").fetchone()[0]
            if size > MAX_SPANS:
                db.execute("DELETE FROM trace_spans WHERE rowid IN (SELECT rowid FROM trace_spans ORDER BY started,rowid LIMIT ?)",
                           (size-MAX_SPANS,))

    def prune(self):
        with self.store.connect() as db:
            db.execute("DELETE FROM trace_spans WHERE started < ?", (time.time_ns()-TTL_NS,))

    def raw(self, trace):
        with self.store.read() as db:
            rows = db.execute("SELECT * FROM trace_spans WHERE trace=? AND started>=? ORDER BY started LIMIT 1001",
                              (trace,time.time_ns()-TTL_NS)).fetchall()
        if not rows:
            raise NoTrace
        resources = {}
        for row in rows:
            span = {"spanId": row["span"], "name": row["name"], "startTimeUnixNano": str(row["started"]),
                    "endTimeUnixNano": str(row["ended"]), "status": {"code": row["status"]}}
            if row["parent"]:
                span["parentSpanId"] = row["parent"]
            if row["kind"] is not None:
                span["kind"] = row["kind"]
            if row["thread_cpu_ns"] is not None:
                span["attributes"] = [{"key":"nebula.thread_cpu_ns","value":{"intValue":str(row["thread_cpu_ns"])}}]
            if row["http_status"] is not None:
                span["attributes"] = [{"key":"http.response.status_code","value":{"intValue":str(row["http_status"])}}]
            resources.setdefault((row["service"], row["instance"], row["environment"]), []).append(span)
        keys = ("service.name", "service.instance.id", "deployment.environment.name")
        return {"resourceSpans": [{"resource": {"attributes": [
                    {"key": key, "value": {"stringValue": value}}
                    for key, value in zip(keys, resource) if value is not None]},
                    "scopeSpans": [{"spans": spans}]} for resource, spans in resources.items()]}

    def search(self, start, end, operation, failed, limit, keyword="", min_ms=0, slowest=False, service="",
               status_class=""):
        where = ["started BETWEEN ? AND ?"]
        args = [max(start, time.time_ns()-TTL_NS), end]
        # In the default list choose the root. With filters, also match a specific child.
        if not (operation or failed or keyword or min_ms or status_class):
            # Keep the HTTP entry even when its remote parent has not arrived yet.
            roots = sorted(ROOT_OPERATIONS)
            where.append("(parent='' OR (service='nebula-webserver' AND name IN (" +
                         ",".join("?" for _ in roots) + ")))")
            args.extend(roots)
        if operation:
            where.append("name = ?")
            args.append(operation)
        if service:
            where.append("service = ?")
            args.append(service)
        if failed:
            where.append("status = 2")
        if status_class:
            family = int(status_class[0]) * 100
            where.append("http_status >= ? AND http_status < ?")
            args.extend((family, family+100))
        if keyword:
            where.append("instr(lower(service || ' ' || name),lower(?)) > 0")
            args.append(keyword)
        if min_ms:
            where.append("ended - started >= ?")
            args.append(int(min_ms * 1e6))
        sort = "ended-started DESC,started DESC" if slowest else "started DESC"
        with self.store.read() as db:
            rows = db.execute(
                "SELECT trace, span, name, service, started, ended, status, http_status FROM ("
                "SELECT *, row_number() OVER (PARTITION BY trace ORDER BY (parent='') DESC,started) AS priority "
                "FROM trace_spans WHERE "+" AND ".join(where)+
                ") WHERE priority=1 ORDER BY "+sort+" LIMIT ?", (*args, limit),
            ).fetchall()
        return [{"traceId": r["trace"], "spanId": r["span"], "name": r["name"], "service": r["service"],
                 "startedAt": str(r["started"]), "durationMs": (r["ended"]-r["started"])/10**6,
                 "error": r["status"]==2, "httpStatus": r["http_status"]} for r in rows]
