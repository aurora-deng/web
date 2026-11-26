"""Offline, stdlib-only tool admission and runtime contracts. No live service access."""
import copy
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
from contextlib import closing, contextmanager
from unittest.mock import patch
from datetime import datetime, timedelta, timezone
import hashlib
import json
from pathlib import Path
import re
import sqlite3
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from backend.agent.tool_runtime.registry import (Effect, Registry, SkillSpec, ToolSpec,
                                                default_registry, obj, ID, text)
from backend.agent.tool_runtime.runtime import (MemoryAuditSink, SqliteAuditSink, ToolError,
                                               ToolRuntime, encode, planner_mode)
from backend.agent.tool_runtime.schema import SchemaError, check_schema, validate
from backend.agent.tool_runtime.sources import Binding, SourceValue, observed


TEST_BASE = (ROOT / "build/tool-runtime-tests").resolve()
TEST_BASE.mkdir(parents=True, exist_ok=True)


@contextmanager
def connection(path, **kwargs):
    with closing(sqlite3.connect(path, **kwargs)) as db:
        with db:
            yield db


@contextmanager
def test_folder():
    # Preserve small isolated test databases; never recursively remove directories.
    folder = Path(tempfile.mkdtemp(prefix="contract-", dir=TEST_BASE)).resolve()
    if not folder.is_relative_to(TEST_BASE):
        raise RuntimeError("test path escaped workspace")
    yield str(folder)


class AtomicFixture:
    """Test-only source: receipt AND business row share one SQLite transaction."""
    def __init__(self, path):
        self.path = path
        self.fail_before_commit = False
        with connection(path) as db:
            db.execute("CREATE TABLE IF NOT EXISTS receipt (actor TEXT, tool TEXT, key TEXT, payload TEXT, result TEXT, PRIMARY KEY(actor,tool,key))")
            db.execute("CREATE TABLE IF NOT EXISTS business (actor TEXT, text TEXT)")

    def execute_once(self, tool, parameters, context):
        payload = encode(parameters).decode()
        with connection(self.path, timeout=3) as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT payload,result FROM receipt WHERE actor=? AND tool=? AND key=?",
                             (context.actor_id, tool, parameters["idempotencyKey"])).fetchone()
            if row:
                if row[0] != payload:
                    raise ToolError("conflict")
                return observed(json.loads(row[1]), "test.store")
            db.execute("INSERT INTO business VALUES (?,?)", (context.actor_id, parameters["text"]))
            result = {"stored": True}
            db.execute("INSERT INTO receipt VALUES (?,?,?,?,?)",
                       (context.actor_id, tool, parameters["idempotencyKey"], payload, encode(result).decode()))
            if self.fail_before_commit:
                raise RuntimeError("simulated abort; this detail must not escape")
        return observed(result, "test.store")


class Admission(unittest.TestCase):
    def setUp(self):
        self.registry = default_registry()

    def test_catalog_and_bounds(self):
        self.assertEqual(len(self.registry.tools), 18)
        self.assertEqual(len(self.registry.skills), 19)
        self.assertNotIn("fleet.nav.dispatch", self.registry.skills)
        self.assertEqual(self.registry.tool("fleet.list_tasks").parameters["properties"]["limit"]["maximum"], 16)
        self.assertEqual(self.registry.tool("material.search").parameters["properties"]["limit"]["maximum"], 20)
        for name in ("evidence.get", "material.chunk"):
            self.assertEqual(self.registry.tool(name).max_bytes, 4096)

    def test_registry_cannot_be_mutated_through_spec_or_view(self):
        spec = self.registry.tool("fleet.get_task_status")
        spec.parameters["additionalProperties"] = True
        view = self.registry.tools
        view["fleet.get_task_status"].parameters["required"].clear()
        self.assertFalse(self.registry.tool(spec.name).parameters["additionalProperties"])
        self.assertEqual(self.registry.tool(spec.name).parameters["required"], ["taskId"])
        with self.assertRaises(TypeError):
            view["new"] = spec

    def test_bad_admission(self):
        good = ToolSpec("test.read", obj({"taskId": ID}), 1024)
        bads = [replace(good, max_bytes=0), replace(good, max_bytes=None),
                replace(good, parameters={"type": "object", "properties": {}, "required": []}),
                replace(good, effect=Effect.EXTERNAL), replace(good, effect=Effect.PROPOSAL),
                replace(good, parameters=obj({"BEGIN": ID})), replace(good, name="database.query"),
                replace(good, parameters=obj({"url": text()})),
                replace(good, parameters=obj({"sql": text()})), replace(good, timeout_ms=0),
                replace(good, parameters=obj({"x": {"type": "string"}})),
                replace(good, parameters=obj({"x": {"type": "array", "items": ID}})),
                replace(good, parameters=obj({"x": {"type": "integer"}})),
                replace(good, parameters=obj({"x": {"type": "null"}})),
                replace(good, parameters=obj({"x": dict(ID, format="uri")}))]
        for bad in bads:
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                Registry().add_tool(bad)

    def test_write_key_must_be_required_nonempty_string(self):
        good = self.registry.tool("memory.write")
        for schema in (obj({"idempotencyKey": ID}, []), obj({"idempotencyKey": {"type": "boolean"}}),
                       obj({"idempotencyKey": {"type": "string", "maxLength": 128}})):
            with self.assertRaises(ValueError):
                Registry().add_tool(replace(good, parameters=schema))

    def test_proposal_requires_human_approval(self):
        for skill in self.registry.skills.values():
            if skill.effect == Effect.PROPOSAL:
                self.assertIn("human_approval", skill.preconditions)
        spec = SkillSpec("test.propose", Effect.PROPOSAL, obj({}), ("authorized_actor",), 1000)
        with self.assertRaises(ValueError):
            Registry().add_skill(spec)

    def test_native_catalog_matches_python_constraints(self):
        from scripts.project.generate_skill_catalog import generate, OUTPUT
        self.assertEqual(OUTPUT.read_text(encoding="utf-8"), generate())
        for spec in self.registry.skills.values():
            check_schema(spec.parameters)
        self.assertNotIn("fleet.task.inspect", self.registry.skills)



class Runtime(unittest.TestCase):
    def setUp(self):
        self.registry = default_registry()
        self.audit = MemoryAuditSink()
        self.runtime = ToolRuntime(self.registry, self.audit)

    def bind(self, name, value, **kwargs):
        self.runtime.bind(name, Binding("test.source", lambda actor, tool, params: actor == "owner",
            read=lambda params, context: SourceValue(copy.deepcopy(value), "test.source",
                kwargs.get("read_at", datetime.now(timezone.utc)), kwargs.get("fresh", True))))

    def call(self, name="fleet.get_task_status", params=None, **kwargs):
        return self.runtime.call(name, {"taskId": "task-1"} if params is None else params,
                                 actor_id=kwargs.get("actor", "owner"), trace_id=kwargs.get("trace", "trace-1"))

    def test_unbound_and_unauthorized_are_denied(self):
        self.assertEqual(self.call()["error"]["code"], "denied")
        self.bind("fleet.get_task_status", {})
        self.assertEqual(self.call(actor="other")["error"]["code"], "denied")
        self.assertTrue(self.call()["ok"])

    def test_input_rejection_before_source(self):
        calls = []
        self.runtime.bind("fleet.get_task_status", Binding("test.source", lambda *a: True,
                          read=lambda *a: calls.append(1)))
        for params in ({"taskId": "../secret"}, {"taskId": "/absolute"}, {"taskId": "a\\b"},
                       {"taskId": "task", "source": "forged"}, {"taskId": "task", "fresh": True},
                       {"taskId": 3}, {}, {"taskId": "a\nb"}, {"taskId": "a" * 65}):
            with self.subTest(params=params):
                self.assertEqual(self.call(params=params)["error"]["code"], "invalid")
        self.assertFalse(calls)

    def test_limit_metric_and_symmetric_context_rules(self):
        for name, params in [("fleet.list_tasks", {"limit": 51}), ("fleet.list_tasks", {"limit": 17}),
                             ("chat.search", {"query": "x", "limit": 51}),
                             ("metrics.get", {"names": ["not.allowed"], "windowSeconds": 30}),
                             ("metrics.get", {"names": ["chat.ack_latency_ms"], "windowSeconds": 86401}),
                             ("chat.get_context", {"conversationId": "a", "messageId": "b", "before": 3, "after": 2}),
                             ("quiz.next", {"subject": "math", "excludeMastered": False}),
                             ("material.chunk", {"materialId": "m", "anchor": {"kind": "page", "offset": 0}}),
                             ("progress.write", {"date": "2026-02-31", "subject": "math", "itemId": "one", "minutes": 30, "idempotencyKey": "key"})]:
            with self.subTest(name=name):
                self.assertEqual(self.call(name, params)["error"]["code"], "invalid")

    def test_unicode_reply_is_byte_bounded(self):
        self.bind("evidence.get", {"text": "??" * 10000})
        result = self.call("evidence.get", {"evidenceId": "one"})
        self.assertTrue(result["ok"])
        self.assertTrue(result["truncated"])
        self.assertLessEqual(len(encode(result)), 4096)
        self.assertEqual(planner_mode([result]), "rules")

    def test_collection_cannot_hide_bound_in_object(self):
        self.bind("fleet.list_tasks", {"items": list(range(99))})
        self.assertEqual(self.call("fleet.list_tasks", {"limit": 2})["error"]["code"], "invalid")

    def test_collection_is_limited_by_requested_count(self):
        self.bind("fleet.list_tasks", list(range(100)))
        result = self.call("fleet.list_tasks", {"limit": 3})
        self.assertEqual(result["value"], [0, 1, 2])
        self.assertTrue(result["truncated"])

    def test_context_bound_follows_radius(self):
        self.bind("chat.get_context", list(range(41)))
        result = self.call("chat.get_context", {"conversationId": "a", "messageId": "b", "radius": 2})
        self.assertEqual(len(result["value"]), 5)

    def test_material_search_is_metadata_only(self):
        self.bind("material.search", [{"materialId": "one", "text": "not metadata"}])
        self.assertEqual(self.call("material.search", {"query": "x", "subject": "systems", "type": "note", "limit": 1})["error"]["code"], "invalid")

    def test_chunk_preserves_anchor(self):
        anchor = {"kind": "page", "offset": 1}
        self.bind("material.chunk", {"materialId": "m", "anchor": anchor, "text": "?" * 10000})
        result = self.call("material.chunk", {"materialId": "m", "anchor": anchor})
        self.assertTrue(result["ok"])
        self.assertTrue(result["truncated"])
        self.assertEqual(result["value"]["anchor"], anchor)
        self.assertEqual(result["value"]["materialId"], "m")
        self.assertLessEqual(len(encode(result)), 4096)

    def test_missing_or_forged_chunk_anchor_is_invalid(self):
        self.bind("material.chunk", {"materialId": "m", "text": "no anchor"})
        self.assertEqual(self.call("material.chunk", {"materialId": "m", "anchor": {"kind": "time", "offset": 10}})["error"]["code"], "invalid")

    def test_stale_evidence_cannot_reach_provider(self):
        self.bind("fleet.get_task_status", {}, fresh=False)
        result = self.call()
        self.assertTrue(result["ok"])
        self.assertFalse(result["fresh"])
        self.assertEqual(planner_mode([result]), "rules")
        self.assertEqual(planner_mode([]), "rules")

    def test_age_overrides_fresh_flag(self):
        self.bind("fleet.get_task_status", {}, read_at=datetime.now(timezone.utc) - timedelta(seconds=31))
        self.assertFalse(self.call()["fresh"])

    def test_future_time_and_forged_source_are_invalid(self):
        self.bind("fleet.get_task_status", {}, read_at=datetime.now(timezone.utc) + timedelta(days=1))
        self.assertEqual(self.call()["error"]["code"], "invalid")
        runtime = ToolRuntime(self.registry, self.audit)
        runtime.bind("fleet.get_task_status", Binding("test.source", lambda *a: True, read=lambda *a: observed({}, "forged")))
        self.assertEqual(runtime.call("fleet.get_task_status", {"taskId": "x"}, actor_id="owner", trace_id="t")["error"]["code"], "invalid")

    def test_audit_redacts_values(self):
        self.bind("fleet.get_task_status", {})
        self.call(params={"taskId": "private-task"})
        record = self.audit.records[-1]
        self.assertEqual(record.tool, "fleet.get_task_status")
        self.assertEqual(record.trace_id, "trace-1")
        self.assertEqual(record.parameter_summary, {"taskId": "str"})
        self.assertEqual(record.result_code, "ok")
        self.assertGreaterEqual(record.elapsed_ms, 0)
        self.assertNotIn("private-task", str(self.audit.records))
        self.assertNotIn("owner", str(self.audit.records))

    def test_audit_failure_denies_before_handler(self):
        audit = MemoryAuditSink(capacity=1)
        runtime = ToolRuntime(self.registry, audit)
        calls = []
        runtime.bind("fleet.get_task_status", Binding("test.source", lambda *a: True,
            read=lambda *a: calls.append(1) or observed({}, "test.source")))
        runtime.call("fleet.get_task_status", {"taskId": "x"}, actor_id="owner", trace_id="t")
        runtime.call("fleet.get_task_status", {"taskId": "x"}, actor_id="owner", trace_id="t")
        self.assertEqual(len(calls), 1)

    def test_sqlite_audit_persists(self):
        with test_folder() as folder:
            sink = SqliteAuditSink(str(Path(folder) / "audit.db"))
            runtime = ToolRuntime(self.registry, sink)
            runtime.call("missing.tool", {}, actor_id="owner", trace_id="t")
            with connection(sink.database) as db:
                record = json.loads(db.execute("SELECT record FROM tool_audit").fetchone()[0])
            self.assertEqual(record["result_code"], "not_found")

    def test_deadline_is_propagated_and_late_result_discarded(self):
        r = Registry()
        r.add_tool(ToolSpec("test.read", obj({}), 512, timeout_ms=1))
        runtime = ToolRuntime(r, self.audit)
        clock = [100.0]
        def late(params, context):
            self.assertEqual(context.deadline, 100.001)
            clock[0] += 0.002
            return observed({}, "test.source")
        runtime.bind("test.read", Binding("test.source", lambda *a: True, read=late))
        with patch("backend.agent.tool_runtime.runtime.time.monotonic", side_effect=lambda: clock[0]):
            result = runtime.call("test.read", {}, actor_id="owner", trace_id="t")
        self.assertEqual(result["error"]["code"], "timeout")

    def test_classified_errors_and_no_exception_disclosure(self):
        for error, expected in [(ToolError("not_found"), "not_found"), (ToolError("denied"), "denied"),
                                (TimeoutError("private endpoint"), "timeout"),
                                (RuntimeError("private credential"), "unavailable")]:
            runtime = ToolRuntime(self.registry, self.audit)
            def fail(*args):
                raise error
            runtime.bind("fleet.get_task_status", Binding("test.source", lambda *a: True, read=fail))
            result = runtime.call("fleet.get_task_status", {"taskId": "x"}, actor_id="owner", trace_id="t")
            self.assertEqual(result["error"]["code"], expected)
            self.assertNotIn("private", str(result))

    def test_write_rejects_plain_handler(self):
        with self.assertRaises(ValueError):
            self.runtime.bind("memory.write", Binding("test.store", lambda *a: True, read=lambda *a: None))

    def test_bad_actor_trace_and_name_are_not_echoed(self):
        for kwargs in ({"actor": "owner\nsecret"}, {"trace": "t\nsecret"}):
            result = self.call(**kwargs)
            self.assertEqual(result["error"]["code"], "invalid")
        result = self.call("bad\nsecret", {})
        self.assertEqual(result["error"]["code"], "invalid")
        self.assertNotIn("secret", str(self.audit.records))

    def test_non_json_source_is_invalid(self):
        self.bind("fleet.get_task_status", {"x": float("nan")})
        self.assertEqual(self.call()["error"]["code"], "invalid")


class Idempotency(unittest.TestCase):
    def test_replay_conflict_restart_and_owner_scope(self):
        with test_folder() as folder:
            path = str(Path(folder) / "source.db")
            def runtime():
                result = ToolRuntime(default_registry(), MemoryAuditSink())
                result.bind("memory.write", Binding("test.store", lambda *a: True, write=AtomicFixture(path)))
                return result
            def call(rt, text="remember", actor="owner"):
                return rt.call("memory.write", {"text": text, "idempotencyKey": "stable-key"}, actor_id=actor, trace_id="t")
            rt = runtime()
            first = call(rt)
            self.assertTrue(first["ok"])
            self.assertEqual(call(rt)["value"], first["value"])
            self.assertEqual(call(runtime())["value"], first["value"])
            self.assertEqual(call(rt, text="different")["error"]["code"], "conflict")
            self.assertTrue(call(rt, actor="other")["ok"])
            with connection(path) as db:
                self.assertEqual(db.execute("SELECT COUNT(*) FROM business").fetchone()[0], 2)

    def test_atomic_rollback_does_not_leave_side_effect_without_receipt(self):
        with test_folder() as folder:
            path = str(Path(folder) / "source.db")
            port = AtomicFixture(path)
            rt = ToolRuntime(default_registry(), MemoryAuditSink())
            rt.bind("memory.write", Binding("test.store", lambda *a: True, write=port))
            def call():
                return rt.call("memory.write", {"text": "x", "idempotencyKey": "key"}, actor_id="owner", trace_id="t")
            port.fail_before_commit = True
            self.assertEqual(call()["error"]["code"], "unavailable")
            port.fail_before_commit = False
            self.assertTrue(call()["ok"])
            with connection(path) as db:
                self.assertEqual(db.execute("SELECT COUNT(*) FROM business").fetchone()[0], 1)
                self.assertEqual(db.execute("SELECT COUNT(*) FROM receipt").fetchone()[0], 1)

    def test_concurrent_same_key_has_one_effect(self):
        with test_folder() as folder:
            path = str(Path(folder) / "source.db")
            rt = ToolRuntime(default_registry(), MemoryAuditSink())
            rt.bind("memory.write", Binding("test.store", lambda *a: True, write=AtomicFixture(path)))
            with ThreadPoolExecutor(max_workers=4) as pool:
                results = list(pool.map(lambda _: rt.call("memory.write", {"text": "x", "idempotencyKey": "key"}, actor_id="owner", trace_id="t"), range(8)))
            self.assertTrue(all(result["ok"] for result in results))
            with connection(path) as db:
                self.assertEqual(db.execute("SELECT COUNT(*) FROM business").fetchone()[0], 1)


if __name__ == "__main__":
    unittest.main()
