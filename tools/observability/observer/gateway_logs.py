"""Read bounded Envoy access records and expose the ingress measurements."""

from bisect import bisect_left
from collections import Counter
import json
import os
from pathlib import Path
import re
import sqlite3
import stat
import threading
import time

from .events import Invalid, hex_id
from .store import Conflict


SOURCE = "nebula-gateway"
ROUTES = frozenset({"other", "fast", "health_live", "health_ready", "health_version", "page", "auth_login",
                    "auth_register", "conversations", "tree", "ws", "sse", "api", "files", "deny"})
OPERATIONS = {route: f"gateway.{route}" for route in ROUTES}
TIMING_FIELDS = frozenset({"durationUs", "upstreamConnectUs", "upstreamHeaderUs", "upstreamResponseUs"})
STAGES = {"durationUs": "request", "upstreamHeaderUs": "upstream_header", "upstreamResponseUs": "upstream_response"}
FIELDS = frozenset({"at", "requestId", "route", "status", "details", "traceId", "elapsedMs", *TIMING_FIELDS})
INCIDENTS = {"upstream_response_timeout": "upstream_timeout", "stream_idle_timeout": "idle_timeout",
             "no_healthy_upstream": "no_healthy_upstream", "route_not_found": "route_not_found",
             "direct_response": "route_policy", "local_rate_limited": "rate_limited"}
BUCKETS_US = (1000, 2500, 5000, 10000, 25000, 50000, 100000, 250000, 500000, 1000000,
              2500000, 5000000, 15000000, 60000000)
TIMESTAMP = re.compile(r"([0-9]{1,12})(?:\.([0-9]{1,3}))?\Z")
REQUEST_ID = re.compile(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\Z")


def timestamp_ns(value):
    match = TIMESTAMP.fullmatch(value)
    if match is None:
        raise Invalid("网关日志时间戳格式无效")
    return int(match[1]) * 10**9 + int((match[2] or "").ljust(9, "0"))


def duration_us(value, factor=1):
    if not re.fullmatch(r"[0-9]{1,12}", value) or int(value) * factor > 86400 * 10**6:
        raise Invalid("网关耗时超出范围")
    return int(value) * factor


def valid_gateway_request_id(value):
    return isinstance(value, str) and bool(REQUEST_ID.fullmatch(value)) and hex_id(value.replace("-", ""), 32)


def normalize_access(row, now, retention):
    if not isinstance(row, dict) or row.keys() != FIELDS or not all(isinstance(v, str) for v in row.values()):
        raise Invalid("网关访问日志字段无效")
    route, request_id = row["route"], row["requestId"]
    if route == "-":
        route = "other"
    if route not in ROUTES or not valid_gateway_request_id(request_id):
        raise Invalid("网关路由或请求编号无效")
    stamp = timestamp_ns(row["at"])
    if not now - retention <= stamp <= now + 60 * 10**9:
        raise Invalid("网关访问日志时间超出保留范围")
    if not re.fullmatch(r"[1-5][0-9]{2}|0|-", row["status"]):
        raise Invalid("网关 HTTP 状态无效")
    status = int(row["status"]) if row["status"] != "-" else 0
    code = ("gateway.request.failed" if status >= 500 else
            "gateway.request.interrupted" if status in (0, 499) else
            "gateway.request.rejected" if status >= 400 else "gateway.request.completed")
    record = {"schemaVersion": 1, "recordId": request_id, "requestId": request_id,
              "timeUnixNano": str(stamp), "component": "gateway", "operation": OPERATIONS[route],
              "code": code, "level": "ERROR" if status >= 500 else "WARN" if status == 0 or status >= 400 else "INFO",
              "status": status}
    for field in TIMING_FIELDS:
        if row[field] != "-":
            record[field] = duration_us(row[field])
    if "durationUs" not in record and row["elapsedMs"] != "-":
        record["durationUs"] = duration_us(row["elapsedMs"], factor=1000)
    if row["details"] == "via_upstream" and status >= 100:
        record["upstreamStatus"] = status
    if row["details"] in INCIDENTS:
        record["gatewayOutcome"] = INCIDENTS[row["details"]]
    if row["traceId"] != "-":
        if not hex_id(row["traceId"], 32):
            raise Invalid("上游 Trace ID 无效")
        record["traceId"] = row["traceId"]
    return route, record


class GatewayTelemetry:
    def __init__(self, logs, path):
        self.logs = logs
        self.path = Path(path)
        self.identity = None
        self.offset = 0
        self.discard_line = False
        self.lock = threading.Lock()
        self.counts = Counter()
        self.histograms = {}
        self.rejected = 0
        self.log_dropped = 0
        self.pending_bytes = 0
        self.done = threading.Event()
        # Lifetime counters start when the Observer starts. Retain already
        # indexed historical logs without replaying old traffic into new rates.
        try:
            with self.open_log() as log:
                meta = os.fstat(log.fileno())
                self.identity = (meta.st_dev, meta.st_ino)
                self.offset = meta.st_size
        except FileNotFoundError:
            pass
        self.worker = threading.Thread(target=self.collect, name="observer-gateway", daemon=True)
        self.worker.start()

    def open_log(self):
        options = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0)
        descriptor = os.open(self.path, options)
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            os.close(descriptor)
            raise ValueError("Gateway access log must be a regular file")
        return os.fdopen(descriptor, "rb")

    def read(self):
        rows = []
        rejected = 0
        with self.open_log() as log:
            meta = os.fstat(log.fileno())
            identity = (meta.st_dev, meta.st_ino)
            if identity != self.identity or meta.st_size < self.offset:
                self.identity = identity
                self.offset = 0
                self.discard_line = False
            log.seek(self.offset)
            for _ in range(256):
                begin = log.tell()
                line = log.readline(2049)
                if not line:
                    break
                complete = line.endswith(b"\n")
                if len(line) > 2048:
                    rejected += int(not self.discard_line)
                    self.discard_line = not complete
                elif not complete:
                    log.seek(begin)
                    break
                elif self.discard_line:
                    self.discard_line = False
                else:
                    try:
                        rows.append(normalize_access(json.loads(line), time.time_ns(), self.logs.store.retention))
                    except (ValueError, UnicodeError, Invalid):
                        rejected += 1
            self.offset = log.tell()
            pending_bytes = max(0, meta.st_size - self.offset)
        return rows, rejected, pending_bytes

    def index_batch(self):
        try:
            rows, rejected, pending_bytes = self.read()
        except FileNotFoundError:
            rows, rejected, pending_bytes = [], 0, 0
        except (OSError, ValueError):
            rows, rejected, pending_bytes = [], 1, 0
        selected = []
        with self.lock:
            self.pending_bytes = pending_bytes
            self.rejected += rejected
            for route, row in rows:
                response = row["status"]
                origin = "upstream" if "upstreamStatus" in row else "gateway"
                self.counts[route, f"{response // 100}xx" if response else "incomplete", origin] += 1
                if response and route not in ("ws", "sse"):
                    self.observe_timings(row)
                if response == 0 or response >= 400 or "traceId" in row:
                    selected.append(row)
        if selected:
            try:
                self.logs.append(SOURCE, selected)
            except (OSError, Conflict, Invalid, sqlite3.OperationalError):
                with self.lock:
                    self.log_dropped += len(selected)
        return len(rows) + rejected >= 256 and pending_bytes > 0

    def observe_timings(self, row):
        for field, stage in STAGES.items():
            if field not in row:
                continue
            buckets, total = self.histograms.setdefault(stage, ([0] * (len(BUCKETS_US) + 1), 0))
            value = row[field]
            buckets[bisect_left(BUCKETS_US, value)] += 1
            self.histograms[stage] = buckets, total + value

    def collect(self):
        while not self.done.is_set():
            self.done.wait(.05 if self.index_batch() else .5)

    def metrics(self):
        with self.lock:
            rejected, dropped, pending = self.rejected, self.log_dropped, self.pending_bytes
            counts = list(self.counts.items())
            histograms = {stage: (buckets.copy(), total) for stage, (buckets, total) in self.histograms.items()}
        lines = ["# TYPE nebula_gateway_log_rejected_total counter", f"nebula_gateway_log_rejected_total {rejected}",
                 "# TYPE nebula_gateway_log_dropped_total counter", f"nebula_gateway_log_dropped_total {dropped}",
                 "# TYPE nebula_gateway_log_pending_bytes gauge", f"nebula_gateway_log_pending_bytes {pending}",
                 "# TYPE nebula_gateway_access_total counter"]
        for (route, group, origin), count in sorted(counts):
            lines.append(f'nebula_gateway_access_total{{route="{route}",status_class="{group}",origin="{origin}"}} {count}')
        name = "nebula_gateway_http_seconds"
        lines.append(f"# TYPE {name} histogram")
        for stage in STAGES.values():
            buckets, total = histograms.get(stage, ([0] * (len(BUCKETS_US) + 1), 0))
            cumulative = 0
            boundaries = [str(value / 10**6) for value in BUCKETS_US] + ["+Inf"]
            for boundary, count in zip(boundaries, buckets):
                cumulative += count
                lines.append(f'{name}_bucket{{stage="{stage}",le="{boundary}"}} {cumulative}')
            lines.append(f'{name}_count{{stage="{stage}"}} {cumulative}')
            lines.append(f'{name}_sum{{stage="{stage}"}} {total / 10**6:.6f}')
        return "\n".join(lines) + "\n"

    def close(self):
        self.done.set()
        self.worker.join(timeout=3)
