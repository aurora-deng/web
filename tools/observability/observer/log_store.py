"""Bounded structured diagnostics. Arbitrary exception text and request bodies never enter this store."""
import json
import re
import time

from .events import Invalid, identifier, hex_id
from .store import Conflict, encode
from .operation_catalog import HTTP_OPERATIONS, FUNCTIONS
from .gateway_logs import INCIDENTS, OPERATIONS as GATEWAY_OPERATIONS, TIMING_FIELDS, valid_gateway_request_id

LEVELS = ("TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL")
MESSAGES = {
    "observer.started": "Observer 已启动",
    "observer.stopped": "Observer 正在停止",
    "observer.request": "监控查询完成",
    "observer.invalid_request": "监控请求被校验拒绝",
    "observer.source_unavailable": "监控数据源查询失败",
    "observer.store_unavailable": "监控存储暂不可用",
    "observer.internal_error": "监控任务发生未处理异常",
    "observer.collection_failed": "后台采集暂未完成",
    "business.operation.completed": "业务操作完成",
    "business.operation.failed": "业务操作失败",
    "business.http.rejected": "业务 HTTP 请求被拒绝",
    "business.http.failed": "业务 HTTP 请求处理失败",
    "business.ws.rejected": "WebSocket 业务命令被拒绝",
    "business.ws.failed": "WebSocket 业务命令处理失败",
    "runtime.started": "业务运行组件已启动",
    "telemetry.span.completed": "已采样调用链完成",
    "telemetry.span.failed": "已采样调用链失败",
    "gateway.request.completed": "入口请求已结束",
    "gateway.request.rejected": "入口或业务拒绝了请求",
    "gateway.request.interrupted": "客户端在响应完成前断开",
    "gateway.request.failed": "入口或上游响应失败",
}
REQUIRED = {"schemaVersion", "recordId", "timeUnixNano", "level", "component", "code"}
OPTIONAL = {"traceId", "spanId", "durationUs", "status", "operation", "sourceFile", "sourceLine",
            "processId", "threadId", "errorCode", "requestId", "errorType"}
GATEWAY_NUMERIC_FIELDS = (TIMING_FIELDS - {"durationUs"}) | {"upstreamStatus"}
GATEWAY_FIELDS = GATEWAY_NUMERIC_FIELDS | {"gatewayOutcome"}
ERROR_TYPES = {"Exception", "ValueError", "TypeError", "RuntimeError", "OSError", "TimeoutError",
               "OperationalError", "Invalid", "Conflict", "Capacity", "BackendError", "JSONDecodeError"}
ERROR_CODES = {"invalid_argument", "not_found", "conflict", "unauthenticated", "forbidden",
               "rate_limited", "unavailable", "internal", "business_failed", "business_rejected"}
OPERATIONS = {"GET /api/conversations", "GET /api/conversations/:id/messages",
              "POST /api/conversations/:id/messages", "WS chat.send", "business HTTP", "business WS",
              "ChatService::conversations", "ChatService::history", "ChatService::historyBefore",
              "ChatService::sendMessage", "PostgreSQL command",
              "PostgresConversationRepository::listConversationSummaries"}
OPERATIONS |= HTTP_OPERATIONS | set(FUNCTIONS.values()) | set(GATEWAY_OPERATIONS.values())


def normalize(record, now, retention):
    gateway = isinstance(record, dict) and record.get("component") == "gateway"
    optional = OPTIONAL | GATEWAY_FIELDS if gateway else OPTIONAL
    if (not isinstance(record, dict) or not REQUIRED <= record.keys() <= REQUIRED | optional
            or type(record.get("schemaVersion")) is not int or record["schemaVersion"] != 1):
        raise Invalid("日志只接受版本 1 的白名单字段，不接收正文、SQL 或异常原文")
    result = dict(record)
    if any(not isinstance(result[key],str) for key in ("level","component","code")):
        raise Invalid("日志级别、组件与事件代码需要字符串")
    if (not isinstance(result["recordId"], str) or not re.fullmatch(r"[a-zA-Z0-9_.:-]{1,96}", result["recordId"])
            or result["level"] not in LEVELS or not identifier(result["component"])
            or result["code"] not in MESSAGES):
        raise Invalid("日志标识、级别、组件或事件代码无效")
    if result["code"].startswith("gateway.") != gateway:
        raise Invalid("网关事件代码须来自网关组件")
    stamp = result["timeUnixNano"]
    if not isinstance(stamp, str) or not stamp.isascii() or not stamp.isdecimal() or len(stamp) > 19:
        raise Invalid("日志时间需要纳秒十进制字符串")
    if not now - retention <= int(stamp) <= now + 60 * 10**9:
        raise Invalid("日志时间超出保留范围")
    for key, width in (("traceId", 32), ("spanId", 16)):
        if key in result and not hex_id(result[key], width):
            raise Invalid("日志关联标识无效")
    if "requestId" in result and not (valid_gateway_request_id(result["requestId"]) if gateway else hex_id(result["requestId"], 16)):
        raise Invalid("日志请求编号无效")
    if "spanId" in result and "traceId" not in result:
        raise Invalid("Span ID 必须同时携带 Trace ID")
    for key, maximum in (("durationUs", 86400 * 10**6), ("status", 599), ("sourceLine", 10**7),
                         ("processId", 2**31 - 1)):
        if key in result and (type(result[key]) is not int or not 0 <= result[key] <= maximum):
            raise Invalid("日志数值超出范围")
    for key in GATEWAY_NUMERIC_FIELDS & result.keys():
        value = result[key]
        low, high = (100, 599) if key == "upstreamStatus" else (0, 86400 * 10**6)
        if type(value) is not int or not low <= value <= high:
            raise Invalid("网关上游计时或状态超出范围")
    if "gatewayOutcome" in result and result["gatewayOutcome"] not in INCIDENTS.values():
        raise Invalid("网关结果分类无效")
    if "threadId" in result and (not isinstance(result["threadId"], str)
                                 or not re.fullmatch(r"[0-9]{1,20}", result["threadId"])):
        raise Invalid("线程标识无效")
    if "operation" in result and (not isinstance(result["operation"],str) or result["operation"] not in OPERATIONS):
        raise Invalid("日志操作须使用规范化业务名称")
    if "errorCode" in result and (not isinstance(result["errorCode"],str) or result["errorCode"] not in ERROR_CODES):
        raise Invalid("日志错误代码无效")
    if "errorType" in result and (not isinstance(result["errorType"],str) or result["errorType"] not in ERROR_TYPES):
        raise Invalid("日志异常类型无效")
    if "sourceFile" in result:
        name = result["sourceFile"]
        if (not isinstance(name, str) or len(name) > 240 or name.startswith(("/", "."))
                or not re.fullmatch(r"[a-zA-Z0-9_./-]+", name) or ".." in name):
            raise Invalid("源码位置必须为项目内相对路径")
    return result


class LogStore:
    def __init__(self, store, capacity=20000):
        self.store, self.capacity = store, capacity
        with store.connect() as db:
            db.executescript("""
              CREATE TABLE IF NOT EXISTS diagnostic_logs(
                id INTEGER PRIMARY KEY, source TEXT NOT NULL, record_id TEXT NOT NULL,
                stamp INTEGER NOT NULL, level TEXT NOT NULL, component TEXT NOT NULL,
                trace TEXT, code TEXT NOT NULL, body TEXT NOT NULL, UNIQUE(source,record_id));
              CREATE INDEX IF NOT EXISTS diagnostic_time ON diagnostic_logs(stamp,id);
              CREATE INDEX IF NOT EXISTS diagnostic_trace ON diagnostic_logs(trace,stamp,id);
              CREATE INDEX IF NOT EXISTS diagnostic_level ON diagnostic_logs(level,stamp,id);
            """)

    def append(self, source, batch):
        if not identifier(source) or not isinstance(batch, list) or not 1 <= len(batch) <= 256:
            raise Invalid("日志每批需有 1 至 256 条，来源必须绑定凭据")
        now = time.time_ns()
        records = [normalize(row, now, self.store.retention) for row in batch]
        inserted = 0
        with self.store.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            for row in records:
                body = encode(row)
                previous = db.execute("SELECT body FROM diagnostic_logs WHERE source=? AND record_id=?",
                                      (source, row["recordId"])).fetchone()
                if previous:
                    if previous[0] != body:
                        raise Conflict("相同来源和日志 ID 的内容冲突")
                    continue
                db.execute("INSERT INTO diagnostic_logs(source,record_id,stamp,level,component,trace,code,body)"
                           " VALUES(?,?,?,?,?,?,?,?)", (source, row["recordId"], int(row["timeUnixNano"]),
                           row["level"], row["component"], row.get("traceId"), row["code"], body))
                inserted += 1
            # Newer rows survive both the age limit and the fixed record budget.
            db.execute("DELETE FROM diagnostic_logs WHERE stamp<?", (now - self.store.retention,))
            db.execute("DELETE FROM diagnostic_logs WHERE id IN (SELECT id FROM diagnostic_logs "
                       "ORDER BY stamp DESC,id DESC LIMIT -1 OFFSET ?)", (self.capacity,))
        return {"inserted": inserted, "duplicates": len(records) - inserted}

    def search(self, start, end, args):
        where, values = ["stamp BETWEEN ? AND ?"], [max(start, time.time_ns()-self.store.retention), end]
        for arg, column in (("source", "source"), ("level", "level"), ("component", "component"), ("trace", "trace")):
            value = args.get(arg, "")
            if not value:
                continue
            if ((arg == "level" and value not in LEVELS) or (arg == "trace" and not hex_id(value, 32))
                    or (arg not in ("level", "trace") and not identifier(value))):
                raise Invalid("日志筛选无效")
            where.append(column + "=?")
            values.append(value)
        term = args.get("q", "")
        if not isinstance(term, str) or len(term) > 120:
            raise Invalid("日志关键词最多 120 字符")
        # Search the safe rendered message as well as the stable code; no free text enters storage.
        matching = [code for code, text in MESSAGES.items() if term.casefold() in (code+text).casefold()]
        if term:
            codes="code IN ("+",".join("?" for _ in matching)+")" if matching else "0"
            where.append("("+codes+" OR instr(lower(body),?)>0 OR instr(lower(source),?)>0)")
            values.extend(matching)
            values.extend((term.casefold(),term.casefold()))
        limit = args.get("limit", "100")
        if not str(limit).isdigit() or not 1 <= int(limit) <= 200:
            raise Invalid("日志每页最多 200 条")
        filters, parameters = list(where), list(values)
        cursor = args.get("cursor", "")
        if cursor:
            if not isinstance(cursor, str) or not re.fullmatch(r"[0-9]{1,19}:[0-9]{1,19}", cursor):
                raise Invalid("日志游标无效")
            stamp, ident = map(int, cursor.split(":"))
            if not start <= stamp <= end or not 0 < ident < 2**63:
                raise Invalid("日志游标超出时间窗口")
            where.append("(stamp,id)<(?,?)")
            values.extend((stamp, ident))
        with self.store.read() as db:
            rows = db.execute("SELECT * FROM diagnostic_logs WHERE " + " AND ".join(where) +
                              " ORDER BY stamp DESC,id DESC LIMIT ?", (*values, int(limit)+1)).fetchall()
            counts = dict(db.execute("SELECT level,count(*) FROM diagnostic_logs WHERE " +
                                    " AND ".join(filters)+" GROUP BY level", parameters).fetchall())
        selected = rows[:int(limit)]
        items = [{**json.loads(r["body"]), "source": r["source"], "message": MESSAGES[r["code"]]} for r in selected]
        following = str(selected[-1]["stamp"])+":"+str(selected[-1]["id"]) if len(rows)>int(limit) else None
        return {"items": items, "next": following, "counts": counts, "status": "ok" if items else "empty"}

    def from_spans(self, payload):
        """Compatibility bridge over already validated OTLP; never reads SQL or arbitrary attributes."""
        records = []
        for group in payload.get("resourceSpans", []):
            service = next((a.get("value", {}).get("stringValue") for a in
                            group.get("resource", {}).get("attributes", []) if a.get("key")=="service.name"), "")
            if service != "nebula-webserver":
                continue
            for scope in group.get("scopeSpans", []):
                for span in scope.get("spans", []):
                    if span.get("name") not in OPERATIONS or span.get("kind") not in (2, "SPAN_KIND_SERVER"):
                        continue
                    failed = span.get("status", {}).get("code") in (2, "STATUS_CODE_ERROR")
                    row = {"schemaVersion": 1, "recordId": span["traceId"]+"."+span["spanId"],
                           "timeUnixNano": str(span["endTimeUnixNano"]), "level": "ERROR" if failed else "INFO",
                           "component": "trace-bridge", "code": "telemetry.span.failed" if failed else "telemetry.span.completed",
                           "traceId": span["traceId"], "spanId": span["spanId"], "operation": span["name"],
                           "durationUs": (int(span["endTimeUnixNano"])-int(span["startTimeUnixNano"]))//1000}
                    records.append(row)
        if records:
            self.append("nebula-traces", records[:256])
