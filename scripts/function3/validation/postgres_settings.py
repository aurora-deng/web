"""Bounded effective PostgreSQL settings from approved isolated benchmark databases.

No rows, secrets, server configuration files, writes, reloads or restarts are read
or performed here. Session tuning is opt-in in the benchmark harness.
"""
from datetime import datetime, timezone
import os
import json
import math
from pathlib import Path
import shlex
import subprocess

try:
    from .measurement_noise import digest
except ImportError:
    from measurement_noise import digest

DATABASES = {"function3_baseline_test", "function3_phase11_test"}
CLUSTER = Path("/home/pikachu/phase11-data/function3/pg")
SOCKET = "/home/pikachu/phase11-data/function3/pg/run"
MARKER = "Function3 disposable test database; never use as formal database\n"
IDENTITY_SQL = ("SELECT current_database(), current_user, current_setting('data_directory'), "
                "current_setting('port'), current_setting('unix_socket_directories'), "
                "current_setting('listen_addresses')")
SETTINGS = (
    "server_version_num", "fsync", "synchronous_commit", "full_page_writes",
    "shared_buffers", "max_connections", "max_wal_size", "checkpoint_timeout",
    "wal_buffers", "wal_compression", "wal_sync_method", "commit_delay",
    "commit_siblings", "work_mem", "effective_cache_size", "track_io_timing",
    "track_wal_io_timing",
)
DURABILITY = ("fsync", "synchronous_commit", "full_page_writes")
SQL = ("SELECT current_database(), name, setting, coalesce(unit,''), context, source, "
       "pending_restart::text FROM pg_settings WHERE name IN (" +
       ",".join("'" + name + "'" for name in SETTINGS) + ") ORDER BY name")


def parse_snapshot(output, expected_database):
    if expected_database not in DATABASES:
        raise ValueError("only isolated benchmark databases are allowed")
    if not isinstance(output, str) or len(output.encode("utf-8")) > 16384:
        raise ValueError("invalid or oversized PostgreSQL settings response")
    values, metadata = {}, {}
    for line in output.splitlines():
        columns = line.split("\t")
        if len(columns) != 7:
            raise ValueError("malformed PostgreSQL settings response")
        database, name, setting, unit, context, source, pending = columns
        if (database != expected_database or name not in SETTINGS or name in values
                or not setting or len(setting) > 128 or pending not in {"true", "false"}):
            raise ValueError("settings identity, value or isolation mismatch")
        values[name] = setting
        metadata[name] = {"unit": unit, "context": context, "source": source,
                          "pendingRestart": pending == "true"}
    if set(values) != set(SETTINGS):
        raise ValueError("incomplete PostgreSQL settings response")
    return {"database": expected_database, "settings": values, "metadata": metadata,
            "settingsSha256": digest(values), "readAt": datetime.now(timezone.utc).isoformat(),
            "source": "isolated.pg_settings", "fresh": True}


def validate_connection(connection, database):
    if database not in DATABASES or not isinstance(connection, str):
        raise ValueError("only isolated benchmark databases are allowed")
    fields = {}
    for part in shlex.split(connection):
        key, separator, value = part.partition("=")
        if not separator or key in fields:
            raise ValueError("invalid benchmark connection")
        fields[key] = value
    required = {"host": SOCKET, "port": "55434", "user": "function3_test_admin", "dbname": database}
    if (any(fields.get(key) != value for key, value in required.items())
            or set(fields) - set(required) - {"options"}):
        raise ValueError("connection is not the approved isolated benchmark endpoint")
    allowed = {"track_wal_io_timing", "track_io_timing", "synchronous_commit", "commit_delay", "commit_siblings"}
    options = shlex.split(fields.get("options", ""))
    if len(options) % 2:
        raise ValueError("invalid session options")
    seen = set()
    for i in range(0, len(options), 2):
        key, separator, value = options[i+1].partition("=")
        if options[i] != "-c" or key not in allowed or not separator or key in seen:
            raise ValueError("unsupported session option")
        seen.add(key)
        if key in {"commit_delay", "commit_siblings"}:
            maximum = 10000 if key == "commit_delay" else 100
            if not value.isascii() or not value.isdecimal() or not 0 <= int(value) <= maximum:
                raise ValueError("session tuning outside bounded range")
        elif value not in {"on", "off"}:
            raise ValueError("invalid session boolean")
    return connection


def psql_environment():
    """No credential files or ambient PostgreSQL connection overrides."""
    environment = {key: os.environ[key] for key in ("PATH", "LD_LIBRARY_PATH", "SYSTEMROOT") if key in os.environ}
    environment.update(PGPASSFILE=os.devnull, PGSERVICEFILE=os.devnull,
                       PGCONNECT_TIMEOUT="2", PGCLIENTENCODING="UTF8", LC_ALL="C")
    return environment


def _query(psql, connection, database, sql, run):
    validate_connection(connection, database)
    command = ("BEGIN READ ONLY; SET LOCAL statement_timeout='2s'; "
               "SET LOCAL lock_timeout='1s'; " + sql + "; COMMIT")
    result = run([str(Path(psql)), "--no-psqlrc", "--no-password", "--quiet", "--tuples-only",
                  "--no-align", "--field-separator=\t", "--set=ON_ERROR_STOP=1",
                  connection + " connect_timeout=2", "--command", command],
                 capture_output=True, text=True, encoding="utf-8", timeout=4,
                 check=False, env=psql_environment())
    if result.returncode:
        raise RuntimeError("isolated PostgreSQL metadata query failed; no configuration was changed")
    if not isinstance(result.stdout, str) or len(result.stdout.encode("utf-8")) > 16384:
        raise ValueError("invalid or oversized PostgreSQL metadata response")
    return result.stdout


def verify_test_cluster(psql, connection, database, run=subprocess.run):
    """Fail closed before migrations/resets, not merely on a familiar DB name."""
    validate_connection(connection, database)
    marker = CLUSTER / "FUNCTION3_TEST_CLUSTER"
    if (CLUSTER.resolve() != CLUSTER or marker.is_symlink() or not marker.is_file()
            or marker.stat().st_size > 256 or marker.read_text(encoding="utf-8") != MARKER
            or any((CLUSTER / name).resolve() != CLUSTER / name for name in ("data", "run"))):
        raise ValueError("the fixed disposable PostgreSQL cluster marker is missing or invalid")
    observed = _query(psql, connection, database, IDENTITY_SQL, run).rstrip("\r\n").split("\t")
    expected = [database, "function3_test_admin", str(CLUSTER / "data"), "55434", SOCKET, ""]
    if observed != expected:
        raise ValueError("PostgreSQL server identity is not the marked socket-only test instance")
    return {"database": database, "source": "isolated.server_identity", "verified": True,
            "identitySha256": digest(expected)}


def capture_settings(psql, connection, database, run=subprocess.run):
    return parse_snapshot(_query(psql, connection, database, SQL, run), database)


def validate_observation(snapshot, database, durable=True):
    """Validate historical observations without pretending their age is live freshness."""
    if (not isinstance(snapshot, dict) or snapshot.get("database") != database or database not in DATABASES
            or snapshot.get("source") != "isolated.pg_settings" or snapshot.get("fresh") is not True):
        raise ValueError("missing isolated PostgreSQL settings provenance")
    values, metadata = snapshot.get("settings"), snapshot.get("metadata")
    if (not isinstance(values, dict) or set(values) != set(SETTINGS)
            or any(not isinstance(v, str) or not v or len(v) > 128 for v in values.values())
            or snapshot.get("settingsSha256") != digest(values)
            or not isinstance(metadata, dict) or set(metadata) != set(SETTINGS)):
        raise ValueError("incomplete PostgreSQL settings observation or digest mismatch")
    for item in metadata.values():
        if (not isinstance(item, dict) or set(item) != {"unit", "context", "source", "pendingRestart"}
                or any(not isinstance(item[k], str) or len(item[k]) > 128 for k in ("unit", "context", "source"))
                or item["pendingRestart"] is not False):
            raise ValueError("invalid PostgreSQL metadata or pending restart")
    if durable and any(values.get(name) != "on" for name in DURABILITY):
        raise ValueError("durability must remain enabled")
    try:
        instant = datetime.fromisoformat(snapshot["readAt"])
    except (KeyError, TypeError, ValueError):
        raise ValueError("invalid PostgreSQL observation timestamp") from None
    if instant.tzinfo is None or instant.utcoffset() is None:
        raise ValueError("PostgreSQL observation timestamp needs a timezone")
    return instant


def record_round_settings(summary, variant, before, after, entry, durable=True):
    """Reject configuration drift; never bless an unobserved tuning candidate."""
    if variant not in {"baseline", "current"}:
        raise ValueError("unknown benchmark variant")
    database = entry.get("database", before.get("database"))
    before_time = validate_observation(before, database, durable)
    after_time = validate_observation(after, database, durable)
    if after_time < before_time:
        raise ValueError("PostgreSQL observation time moved backwards")
    if before["database"] != after["database"] or before["settings"] != after["settings"] or before["metadata"] != after["metadata"]:
        raise ValueError("PostgreSQL settings changed during the measured round")
    if durable and any(before["settings"].get(name) != "on" for name in DURABILITY):
        raise ValueError("durability must remain enabled")
    if any(value["pendingRestart"] for snapshot in (before, after) for value in snapshot["metadata"].values()):
        raise ValueError("pending PostgreSQL restart makes the settings experiment ambiguous")
    values = before["settings"]
    if before["settingsSha256"] != digest(values) or after["settingsSha256"] != digest(values):
        raise ValueError("settings digest mismatch")
    snapshots = summary.setdefault("postgresSettings", {})
    if variant in snapshots and snapshots[variant] != values:
        raise ValueError("PostgreSQL settings changed between rounds of the same variant")
    snapshots[variant] = dict(values)
    entry.update(postgresSettingsSha256=before["settingsSha256"],
                 postgresSettingsAfterSha256=after["settingsSha256"], postgresSettingsStable=True,
                 postgresSettingsObservation={"before": before, "after": after})


# Aggregate counters only: never query statements, account rows or message contents.
ACTIVITY_TABLES = (
    "users", "user_sessions", "conversations", "conversation_members", "messages",
    "outbox_events", "user_presence", "contact_preferences",
)
ACTIVITY_SQL = """SELECT json_build_object(
    'database', current_database(),
    'timing', json_build_object('trackIo',current_setting('track_io_timing'),'trackWalIo',current_setting('track_wal_io_timing')),
    'databaseReset', (SELECT stats_reset::text FROM pg_stat_database WHERE datname=current_database()),
    'walReset', (SELECT stats_reset::text FROM pg_stat_wal),
    'wal', (SELECT json_build_object('records',wal_records,'fullPageImages',wal_fpi,'bytes',wal_bytes) FROM pg_stat_wal),
    'walIo', (SELECT json_build_object('writes',coalesce(sum(writes),0),'writeMilliseconds',coalesce(sum(write_time),0),
        'fsyncs',coalesce(sum(fsyncs),0),'fsyncMilliseconds',coalesce(sum(fsync_time),0))
        FROM pg_stat_io WHERE object='wal' AND context='normal'),
    'ioResets', (SELECT coalesce(json_agg(json_build_object('backend',backend_type,'context',context,'reset',stats_reset::text)
        ORDER BY backend_type,context),'[]'::json) FROM pg_stat_io WHERE object='wal'),
    'tables', (SELECT coalesce(json_object_agg(relname,json_build_object('inserts',n_tup_ins,'updates',n_tup_upd,
        'hotUpdates',n_tup_hot_upd,'deletes',n_tup_del)),'{}'::json)
        FROM pg_stat_user_tables WHERE schemaname='public' AND relname IN (
        'users','user_sessions','conversations','conversation_members','messages','outbox_events','user_presence','contact_preferences'))
)"""


def benchmark_environment(source):
    """Inherited process state cannot enable models, proxies, DB credentials or telemetry."""
    allowed = ("PATH", "LD_LIBRARY_PATH", "LANG", "LC_ALL", "LC_CTYPE", "LC_MESSAGES", "TZ")
    environment = {key: source[key] for key in allowed if key in source}
    environment.update(PGPASSFILE=os.devnull, PGSERVICEFILE=os.devnull,
                       WEB_OBSERVABILITY_OTLP="", WEB_OBSERVABILITY_LOG="off",
                       WEB_BUSINESS_LOG_LEVEL="off", OTEL_SDK_DISABLED="true")
    return environment


def parse_activity(output, database):
    if database not in DATABASES or not isinstance(output, str) or len(output.encode()) > 16384:
        raise ValueError("invalid PostgreSQL activity input")
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result: raise ValueError("duplicate activity key")
            result[key] = value
        return result
    value = json.loads(output, object_pairs_hook=unique)
    if not isinstance(value, dict) or set(value) != {"database", "timing", "databaseReset", "walReset", "wal", "walIo", "ioResets", "tables"} or value["database"] != database:
        raise ValueError("activity identity or shape mismatch")
    if not isinstance(value["timing"], dict) or set(value["timing"]) != {"trackIo", "trackWalIo"} or any(v not in {"on", "off"} for v in value["timing"].values()):
        raise ValueError("invalid IO timing collection settings")
    for name in ("databaseReset", "walReset"):
        if value[name] is not None and (not isinstance(value[name], str) or len(value[name]) > 64):
            raise ValueError("invalid statistics reset marker")
    counters = [(value["wal"], {"records", "fullPageImages", "bytes"}),
                (value["walIo"], {"writes", "writeMilliseconds", "fsyncs", "fsyncMilliseconds"})]
    if not isinstance(value["tables"], dict) or set(value["tables"]) != set(ACTIVITY_TABLES):
        raise ValueError("missing isolated workload table statistics")
    counters += [(v, {"inserts", "updates", "hotUpdates", "deletes"}) for v in value["tables"].values()]
    for group, keys in counters:
        if not isinstance(group, dict) or set(group) != keys:
            raise ValueError("malformed aggregate counters")
        for number in group.values():
            if type(number) not in (int, float) or not math.isfinite(number) or not 0 <= number <= 2**63-1:
                raise ValueError("invalid aggregate counter")
    if not isinstance(value["ioResets"], list) or not 1 <= len(value["ioResets"]) <= 64:
        raise ValueError("missing WAL IO reset identities")
    identities = set()
    for row in value["ioResets"]:
        if not isinstance(row, dict) or set(row) != {"backend", "context", "reset"}:
            raise ValueError("invalid IO reset row")
        if any(not isinstance(row[k], str) or not row[k] or len(row[k]) > 64 for k in row):
            raise ValueError("invalid IO reset field")
        identity = (row["backend"], row["context"])
        if identity in identities: raise ValueError("duplicate IO reset identity")
        identities.add(identity)
    return {"database": database, "source": "isolated.postgresql.aggregate_activity",
            "readAt": datetime.now(timezone.utc).isoformat(), "fresh": True,
            "counters": value, "snapshotSha256": digest(value)}


def capture_activity(psql, connection, database, run=subprocess.run):
    # Explicit local identity check is mandatory before even aggregate inspection.
    verify_test_cluster(psql, connection, database, run)
    return parse_activity(_query(psql, connection, database, ACTIVITY_SQL, run), database)


def activity_delta(before, after):
    if before.get("database") != after.get("database") or before.get("database") not in DATABASES:
        raise ValueError("activity database changed")
    for snapshot in (before, after):
        if snapshot.get("fresh") is not True or snapshot.get("source") != "isolated.postgresql.aggregate_activity":
            raise ValueError("unverified aggregate activity snapshot")
        if digest(snapshot.get("counters")) != snapshot.get("snapshotSha256"):
            raise ValueError("aggregate activity snapshot digest mismatch")
        parse_activity(json.dumps(snapshot["counters"]), snapshot["database"])
    dates = []
    for snapshot in (before, after):
        try:
            stamp = datetime.fromisoformat(snapshot.get("readAt", ""))
        except (ValueError, TypeError):
            raise ValueError("invalid aggregate activity timestamp") from None
        if stamp.tzinfo is None:
            raise ValueError("aggregate activity timestamp has no timezone")
        dates.append(stamp)
    if dates[1] < dates[0]: raise ValueError("aggregate snapshot time moved backwards")
    old, new = before["counters"], after["counters"]
    if any(old[name] != new[name] for name in ("databaseReset", "walReset", "ioResets", "timing")):
        raise ValueError("statistics reset during measured workload")
    def subtract(a, b):
        if set(a) != set(b): raise ValueError("counter shape changed")
        delta = {k: b[k]-a[k] for k in a}
        if any(v < 0 for v in delta.values()): raise ValueError("counter moved backwards")
        return delta
    tables = {name: subtract(old["tables"][name], new["tables"][name]) for name in ACTIVITY_TABLES}
    for row in tables.values():
        if row["hotUpdates"] > row["updates"]: raise ValueError("HOT count exceeds updates")
        row["hotUpdateRatio"] = row["hotUpdates"]/row["updates"] if row["updates"] else None
    return {"wal": subtract(old["wal"], new["wal"]),
            "walIo": subtract(old["walIo"], new["walIo"]), "tables": tables,
            "scope": {"wal": "entire marked isolated cluster; may include concurrent test activity",
                      "tables": "named tables in the verified disposable database"},
            "beforeSha256": before["snapshotSha256"], "afterSha256": after["snapshotSha256"],
            "statisticsFlushWaitSeconds": 1.1, "observerSessionTiming": old["timing"],
            "ioTimingCaveat": "Disabled timing yields unmeasured zero, not zero IO cost; other cluster sessions may use different timing settings",
            "descriptiveOnly": True}
