"""SQLite storage for bounded, deduplicated development telemetry."""
from contextlib import closing, contextmanager
import hashlib
import json
from pathlib import Path
import sqlite3
import time

from .events import Invalid, identifier, utc_ns, validate

DAY_SECONDS = 86400
DAY = DAY_SECONDS * 10**9
WEEK_SECONDS = 7 * DAY_SECONDS
HISTORICAL_STEP = 300
DENSE_METRIC_SECONDS = 6 * 3600


class Conflict(Exception):
    pass


class Capacity(Exception):
    pass


def encode(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False)


class Store:
    def __init__(self, path, retention_days=7, max_bytes=64 * 1024**2):
        if not 1 <= retention_days <= 30 or max_bytes < 1024**2:
            raise ValueError("invalid store limits")
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.retention = retention_days * DAY
        self.retention_seconds = retention_days * DAY_SECONDS
        self.max_bytes = max_bytes
        with self.connect() as db:
            db.executescript("""
              CREATE TABLE IF NOT EXISTS events(
                id INTEGER PRIMARY KEY AUTOINCREMENT, source TEXT NOT NULL,
                event_id TEXT NOT NULL, kind TEXT NOT NULL, trace TEXT,
                occurred_ns INTEGER NOT NULL, received_ns INTEGER NOT NULL,
                body TEXT NOT NULL, UNIQUE(source,event_id));
              CREATE INDEX IF NOT EXISTS event_time ON events(occurred_ns,id);
              CREATE INDEX IF NOT EXISTS event_trace ON events(trace,occurred_ns,id);
              CREATE TABLE IF NOT EXISTS runs(
                id INTEGER PRIMARY KEY AUTOINCREMENT, digest TEXT NOT NULL UNIQUE,
                started_ns INTEGER NOT NULL, received_ns INTEGER NOT NULL, body TEXT NOT NULL);
              CREATE INDEX IF NOT EXISTS run_time ON runs(started_ns,id);
            """)

    @contextmanager
    def connect(self):
        with closing(sqlite3.connect(str(self.path), timeout=1)) as db, db:
            db.row_factory = sqlite3.Row
            db.execute("PRAGMA journal_mode=DELETE")
            db.execute("PRAGMA synchronous=FULL")
            page_size = db.execute("PRAGMA page_size").fetchone()[0]
            db.execute("PRAGMA max_page_count=" + str(self.max_bytes // page_size))
            yield db

    @contextmanager
    def read(self):
        with closing(sqlite3.connect(self.path.resolve().as_uri() + "?mode=ro", uri=True, timeout=1)) as db:
            db.row_factory = sqlite3.Row
            yield db

    def append(self, source, domain, simulated, batch, now=None):
        now = now if now is not None else time.time_ns()
        if not isinstance(batch, list) or not 1 <= len(batch) <= 256:
            raise Invalid("每批须包含 1 至 256 条事件")
        records = [validate(item, now, self.retention, domain) for item in batch]
        inserted = 0
        try:
            with self.connect() as db:
                db.execute("BEGIN IMMEDIATE")
                for item in records:
                    body = encode({**item, "source": source, "simulated": simulated})
                    prior = db.execute("SELECT body FROM events WHERE source=? AND event_id=?",
                                       (source, item["eventId"])).fetchone()
                    if prior is not None:
                        if prior["body"] != body:
                            raise Conflict("同一来源和事件 ID 的内容冲突")
                        continue
                    db.execute("INSERT INTO events(source,event_id,kind,trace,occurred_ns,received_ns,body)"
                               " VALUES (?,?,?,?,?,?,?)",
                               (source, item["eventId"], item["eventType"], item.get("traceId"),
                                int(item["occurredAt"]), now, body))
                    inserted += 1
        except sqlite3.Error as error:
            # Python 3.9 exposes only the message for these SQLite failures.
            if getattr(error, "sqlite_errorname", None) in ("SQLITE_FULL", "SQLITE_BUSY") or str(error) in (
                    "database or disk is full", "database is locked"):
                raise Capacity("事件存储配额已满或正在忙") from error
            raise
        return {"inserted": inserted, "duplicates": len(records) - inserted}

    def events(self, start, end, limit, cursor=None, trace=None, domain=None, source=None):
        filters, args = ["occurred_ns BETWEEN ? AND ?"], [start, end]
        if trace:
            filters.append("trace=?")
            args.append(trace)
        if domain:
            filters.append("kind LIKE ?")
            args.append(domain + ".%")
        if source:
            filters.append("source=?")
            args.append(source)
        if cursor:
            filters.append("(occurred_ns,id) > (?,?)")
            args.extend(cursor)
        args.append(limit + 1)
        with self.connect() as db:
            rows = db.execute("SELECT * FROM events WHERE " + " AND ".join(filters) +
                              " ORDER BY occurred_ns,id LIMIT ?", args).fetchall()
        page = [{"receivedAt": str(r["received_ns"]), **json.loads(r["body"])} for r in rows[:limit]]
        after = [str(rows[limit-1]["occurred_ns"]), rows[limit-1]["id"]] if len(rows) > limit else None
        return {"items": page, "next": after}

    def import_run(self, report):
        required = {"schemaVersion", "scenario", "startedAt", "finishedAt", "identity", "result"}
        if not isinstance(report, dict) or set(report) != required or type(report["schemaVersion"]) is not int or report["schemaVersion"] != 1:
            raise Invalid("实验记录格式错误")
        identity, result = report["identity"], report["result"]
        keys = {"commit", "binarySha256", "configSha256", "workloadSha256", "fixtureSha256", "environmentSha256"}
        if (not identifier(report["scenario"]) or not isinstance(identity, dict)
                or not keys <= identity.keys() <= keys | {"sourceFingerprint"}
                or not isinstance(result, dict)):
            raise Invalid("实验元数据错误")
        for key, val in identity.items():
            width = (40, 64) if key == "commit" else (64,)
            if val is not None and (not isinstance(val, str) or len(val) not in width or
                                    any(c not in "0123456789abcdef" for c in val)):
                raise Invalid("实验摘要无效")
        allowed = {"workers", "operations", "ackSamples", "errors", "ackP50Ms", "ackP95Ms", "ackP99Ms",
                   "operationsPerSecond", "fastQps", "rssKiB", "fileDescriptors", "recoveryMs", "matchingRows"}
        if not result.keys() <= allowed or any(type(v) not in (int, float) or not 0 <= v < 1e15 for v in result.values()):
            raise Invalid("实验指标无效")
        started, finished = utc_ns(report["startedAt"]), utc_ns(report["finishedAt"])
        now = time.time_ns()
        if not now-self.retention <= started <= finished <= now + 300_000_000_000:
            raise Invalid("实验日期范围无效")
        body = encode(report)
        digest = hashlib.sha256(body.encode()).hexdigest()
        with self.connect() as db:
            db.execute("INSERT OR IGNORE INTO runs(digest,started_ns,received_ns,body) VALUES (?,?,?,?)",
                       (digest, started, now, body))
        return digest

    def latest_device_windows(self, since):
        # Limit labels to provisioned sources and a fixed stream vocabulary.
        with self.connect() as db:
            rows = db.execute("SELECT body FROM events WHERE occurred_ns >= ? AND kind='device.window'"
                              " ORDER BY occurred_ns DESC,id DESC LIMIT 256", (since,)).fetchall()
        latest = {}
        for row in rows:
            event = json.loads(row["body"])
            stream = event["fields"]["stream"]
            if stream in ("camera", "imu"):
                latest.setdefault((event["source"], stream), event)
        return latest.values()

    def runs(self, start, end, limit=100):
        with self.connect() as db:
            rows = db.execute("SELECT digest,body FROM runs WHERE started_ns BETWEEN ? AND ?"
                              " ORDER BY started_ns DESC,id DESC LIMIT ?", (start, end, limit))
            return [{"id": r["digest"], **json.loads(r["body"])} for r in rows]

    def run(self, digest):
        with self.connect() as db:
            row = db.execute("SELECT body FROM runs WHERE digest=? AND started_ns>=?",
                             (digest,time.time_ns()-self.retention)).fetchone()
            return json.loads(row["body"]) if row else None

    def run_markers(self, start, end):
        # Include a finish even when a long run started before the visible window.
        with self.connect() as db:
            rows = db.execute("SELECT digest,body FROM runs WHERE started_ns <= ? "
                              "ORDER BY started_ns DESC,id DESC LIMIT 100", (end,)).fetchall()
        markers = []
        for row in rows:
            report = json.loads(row["body"])
            for state, field in (("started","startedAt"),("finished","finishedAt")):
                stamp = utc_ns(report[field])
                if start <= stamp <= end:
                    markers.append({"occurredAt":str(stamp),
                                    "fields":{"state":state,"scenario":report["scenario"],"recordId":row["digest"]}})
        return markers

    def prune(self, now=None):
        now = now if now is not None else time.time_ns()
        cutoff = now - self.retention
        seconds = cutoff // 10**9
        with self.connect() as db:
            tables = {row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            events = db.execute("DELETE FROM events WHERE occurred_ns < ?", (cutoff,)).rowcount
            runs = db.execute("DELETE FROM runs WHERE started_ns < ?", (cutoff,)).rowcount
            for table, column, floor in (
                ("trace_spans", "started", cutoff),
                ("diagnostic_logs", "stamp", cutoff),
                ("metric_samples", "stamp", seconds),
                ("histogram_windows", "stamp", seconds),
                ("resource_snapshots", "stamp", seconds),
                ("resource_profiles", "started", seconds),
                ("source_samples", "ts", seconds),
                ("alert_transitions", "stamp", seconds),
            ):
                if table in tables:
                    db.execute(f"DELETE FROM {table} WHERE {column} < ?", (floor,))
            # Keep the most recent actual observation of each older bucket.
            # The full six-hour window remains available to SLO calculations.
            for table, dense_seconds in (("metric_samples", DENSE_METRIC_SECONDS),
                                         ("histogram_windows", DENSE_METRIC_SECONDS),
                                         ("resource_snapshots", 3600)):
                if table in tables:
                    boundary = (now // 10**9 - dense_seconds) // HISTORICAL_STEP * HISTORICAL_STEP
                    db.execute(f"DELETE FROM {table} WHERE stamp < ? AND stamp NOT IN ("
                               f"SELECT max(stamp) FROM {table} WHERE stamp < ? GROUP BY stamp/?)",
                               (boundary, boundary, HISTORICAL_STEP))
            if "source_samples" in tables:
                boundary = (now // 10**9 - DENSE_METRIC_SECONDS) // HISTORICAL_STEP * HISTORICAL_STEP
                db.execute("DELETE FROM source_samples WHERE ts < ? AND (source,ts) NOT IN ("
                           "SELECT source,max(ts) FROM source_samples WHERE ts < ? GROUP BY source,ts/?)",
                           (boundary,boundary,HISTORICAL_STEP))
        return events, runs
