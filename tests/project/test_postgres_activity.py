"""Aggregate-only synthetic counters; never connects to PostgreSQL."""
import copy
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.function3.validation import postgres_settings as s


def snapshot(offset=0):
    raw = {"database": "function3_baseline_test", "databaseReset": "2026-11-27 00:00:00+00",
           "walReset": "2026-11-27 00:00:00+00", "timing": {"trackIo": "on", "trackWalIo": "on"}, "wal": {"records": 100+offset, "fullPageImages": 10+offset, "bytes": 10000+offset},
           "walIo": {"writes": 20+offset, "writeMilliseconds": 2+offset, "fsyncs": 10+offset, "fsyncMilliseconds": 5+offset},
           "ioResets": [{"backend": "client backend", "context": "normal", "reset": "2026-11-27 00:00:00+00"}],
           "tables": {name: {"inserts": 20+offset, "updates": 10+2*offset, "hotUpdates": 5+offset, "deletes": offset} for name in s.ACTIVITY_TABLES}}
    return s.parse_activity(json.dumps(raw), "function3_baseline_test")


class PostgresActivityTest(unittest.TestCase):
    def test_delta_reports_hot_and_cluster_scope_without_tuning_claim(self):
        delta = s.activity_delta(snapshot(), snapshot(2))
        self.assertEqual(delta["tables"]["messages"]["updates"], 4)
        self.assertEqual(delta["tables"]["messages"]["hotUpdateRatio"], 0.5)
        self.assertEqual(delta["wal"]["bytes"], 2)
        self.assertIn("entire marked isolated cluster", delta["scope"]["wal"])
        self.assertTrue(delta["descriptiveOnly"])
        self.assertEqual(delta["observerSessionTiming"], {"trackIo": "on", "trackWalIo": "on"})
        self.assertIsNone(s.activity_delta(snapshot(), snapshot())["tables"]["messages"]["hotUpdateRatio"])

    def test_missing_extra_nonfinite_duplicate_and_wrong_database_rejected(self):
        for kind in ("missingTable", "extraTable", "nan", "negative", "boolean", "database", "missingIoReset", "duplicateIoReset"):
            raw = copy.deepcopy(snapshot()["counters"])
            if kind == "missingTable": raw["tables"].pop("messages")
            elif kind == "extraTable": raw["tables"]["private_records"] = raw["tables"]["messages"]
            elif kind == "nan": raw["wal"]["bytes"] = float("nan")
            elif kind == "negative": raw["wal"]["bytes"] = -1
            elif kind == "boolean": raw["wal"]["bytes"] = True
            elif kind == "database": raw["database"] = "phase11"
            elif kind == "missingIoReset": raw["ioResets"] = []
            elif kind == "duplicateIoReset": raw["ioResets"] *= 2
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                s.parse_activity(json.dumps(raw), "function3_baseline_test")
        with self.assertRaises(ValueError): s.parse_activity('{"database":"a","database":"b"}', "function3_baseline_test")
        with self.assertRaises(ValueError): s.parse_activity("x"*16385, "function3_baseline_test")

    def test_resets_decreases_and_snapshot_tampering_rejected(self):
        before = snapshot()
        for kind in ("databaseReset", "walReset", "ioResets", "decrease", "digest", "fresh", "database", "hotOverflow"):
            after = snapshot(1)
            if kind in ("databaseReset", "walReset"): after["counters"][kind] = "different-reset"
            elif kind == "ioResets": after["counters"]["ioResets"][0]["reset"] = "different-reset"
            elif kind == "decrease": after["counters"]["wal"]["records"] = 0
            elif kind == "fresh": after["fresh"] = 1
            elif kind == "database": after["database"] = "phase11"
            elif kind == "hotOverflow": after["counters"]["tables"]["messages"]["hotUpdates"] = 99
            if kind != "digest": after["snapshotSha256"] = s.digest(after["counters"])
            else: after["snapshotSha256"] = "0"*64
            with self.subTest(kind=kind), self.assertRaises(ValueError): s.activity_delta(before, after)

    def test_capture_verifies_identity_before_query(self):
        connection=f"host={s.SOCKET} port=55434 user=function3_test_admin dbname=function3_baseline_test"
        order=[]
        with patch.object(s,"verify_test_cluster",side_effect=lambda *a: order.append("identity")), \
             patch.object(s,"_query",side_effect=lambda *a: order.append("query") or json.dumps(snapshot()["counters"])):
            observed=s.capture_activity("psql",connection,"function3_baseline_test")
        self.assertEqual(order,["identity","query"])
        self.assertEqual(observed["database"],"function3_baseline_test")
        with patch.object(s,"verify_test_cluster",side_effect=ValueError("not isolated")),patch.object(s,"_query") as query:
            with self.assertRaises(ValueError): s.capture_activity("psql",connection,"function3_baseline_test")
            query.assert_not_called()

    def test_sql_is_aggregate_only_and_no_reset_or_business_rows(self):
        sql=s.ACTIVITY_SQL.lower()
        for view in ("pg_stat_io", "pg_stat_wal", "pg_stat_database", "pg_stat_user_tables"):
            self.assertIn(view,sql)
        for forbidden in ("pg_stat_statements","pg_stat_activity","pg_stat_reset","alter system","select *", "from messages", "from users"):
            self.assertNotIn(forbidden,sql)

    def test_sql_parentheses_and_snapshot_times_are_valid(self):
        depth = 0
        quoted = False
        for character in s.ACTIVITY_SQL:
            if character == "'": quoted = not quoted
            elif not quoted and character == "(": depth += 1
            elif not quoted and character == ")": depth -= 1
            self.assertGreaterEqual(depth, 0)
        self.assertEqual(depth, 0)
        before, after = snapshot(), snapshot(1)
        after["readAt"] = "2026-11-27T00:00:00"
        with self.assertRaises(ValueError): s.activity_delta(before, after)
        after["readAt"] = "2020-01-01T00:00:00+00:00"
        with self.assertRaises(ValueError): s.activity_delta(before, after)

    def test_benchmark_environment_is_explicit_and_never_enumerates_secrets(self):
        class Guard(dict):
            def items(self): raise AssertionError("ambient environment enumerated")
        env=s.benchmark_environment(Guard(PATH="tools",DATABASE_PASSWORD="synthetic",WEB_PHASE11_ONNX_MODEL_ROOT="must-not-load",HTTPS_PROXY="must-not-inherit"))
        self.assertNotIn("DATABASE_PASSWORD",env)
        self.assertNotIn("WEB_PHASE11_ONNX_MODEL_ROOT",env)
        self.assertNotIn("HTTPS_PROXY",env)
        self.assertEqual(env["PGPASSFILE"],s.os.devnull)
        self.assertEqual(env["WEB_BUSINESS_LOG_LEVEL"],"off")
        self.assertEqual(env["WEB_OBSERVABILITY_OTLP"],"")


if __name__=="__main__": unittest.main()
