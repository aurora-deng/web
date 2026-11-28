"""Synthetic account fixtures with the real Storage/SQLite transaction implementation.

No real accounts, credential files, network requests or production databases.
"""
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "client/yantu/runtime"))
from backend.agent.tool_runtime.nebula import NebulaWorkspace
from backend.agent.tool_runtime.registry import default_registry, Effect
from backend.agent.tool_runtime.runtime import ToolError

spec = importlib.util.spec_from_file_location("nebula_contract_storage", ROOT / "client/yantu/runtime/server.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class NativeFixture:
    actor = "7"
    def __init__(self): self.calls = []
    def call(self, operation, parameters, deadline):
        self.calls.append((operation, parameters))
        target = {"id": "2", "conversationId": "1", "sequence": 2, "body": "fixture reply", "state": "active"}
        recalled = dict(target, id="3", sequence=3, body="must not be returned", state="recalled")
        return {"items": [target, recalled], "target": target}


class FleetFixture:
    def status(self, identity, deadline): return {"taskId": identity, "phase": "queued"}
    def tasks(self, parameters, deadline): return [{"taskId": "task-1"}][:parameters["limit"]]
    def trace(self, parameters): return {"runId": parameters["runId"], "nodes": []}
    def counts(self): return {"fleet.active_tasks": 2}


class NebulaContracts(unittest.TestCase):
    def setUp(self):
        base = (ROOT / "build-function3/nebula-tool-contracts").resolve()
        base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=base)
        self.root = Path(self.temp.name).resolve()
        if not self.root.is_relative_to(base): raise RuntimeError("test fixture escaped workspace")
        self.addCleanup(self.temp.cleanup)
        self.storage = module.Storage(self.root)
        self.addCleanup(lambda: self.storage.db.close())
        state = {
            "tasks": [{"id": "item-1", "subject": "math", "title": "fixture task"}],
            "notes": [{"id": "note-1", "subjectId": "math", "title": "algebra note", "content": "fixture note", "source": "agent-memory"}],
            "resources": [{"id": "resource-1", "subject": "math", "title": "algebra.pdf", "type": "pdf", "fileId": "file-1"}],
            "flashcards": [{"id": "card-1", "subject": "math", "front": "question", "back": "answer", "mastery": "new"}],
            "focusSessions": [{"id": "focus-1", "subject": "math", "itemId": "item-1", "seconds": 120, "date": "2026-11-27"}],
            "planCapacities": {"dailyMinutes": [30, 90]},
        }
        self.storage.db.execute("UPDATE document SET body=? WHERE id=1", (json.dumps(state),))
        self.storage.db.commit()
        self.native = NativeFixture()
        self.store = NebulaWorkspace(self.storage, self.native, SimpleNamespace(), FleetFixture())

    def ctx(self, actor="7", expired=False):
        return SimpleNamespace(actor_id=actor, deadline=time.monotonic() + (-1 if expired else 5))

    def assert_code(self, code, function, *args):
        with self.assertRaises(ToolError) as caught: function(*args)
        self.assertEqual(caught.exception.code, code)

    def write_parameters(self, tool, key="fixture-key"):
        if tool == "memory.write": return {"text": "fixture memory", "idempotencyKey": key}
        common = {"subject": "math", "itemId": "item-1", "idempotencyKey": key}
        return dict(common, **({"date": "2026-11-27", "minutes": 15} if tool == "progress.write" else {"text": "fixture mistake"}))

    def test_all_fifteen_read_implementations_preserve_sources_and_bounds(self):
        parameters = {
            "fleet.get_task_status": {"taskId": "task-1"}, "fleet.list_tasks": {"limit": 1},
            "evidence.get": {"evidenceId": "task-1"}, "workflow.get_run": {"runId": "run-1", "nodeLimit": 2},
            "metrics.get": {"names": ["fleet.active_tasks"], "windowSeconds": 60},
            "chat.search": {"query": "fixture", "scope": "workspace", "limit": 1},
            "chat.get_context": {"conversationId": "1", "messageId": "2", "radius": 1},
            "chat.tree_node": {"conversationId": "1", "nodeId": "2"}, "memory.read": {"limit": 1},
            "preference.get": {}, "material.search": {"subject": "math", "query": "algebra", "type": "pdf", "limit": 1},
            "material.chunk": {"materialId": "note-1", "anchor": {"kind": "page", "offset": 1}},
            "material.stats": {}, "quiz.next": {"subject": "math", "excludeMastered": True},
            "progress.read": {"subject": "math", "limit": 1},
        }
        self.assertEqual(set(parameters), {name for name, entry in default_registry().tools.items() if entry.effect == Effect.READ})
        results = {}
        for name, values in parameters.items():
            with self.subTest(tool=name):
                result = self.store.read(name, values, self.ctx())
                self.assertTrue(result.source)
                self.assertIsNotNone(result.read_at.tzinfo)
                self.assertIs(result.fresh, name != "workflow.get_run")
                results[name] = result.value
        self.assertEqual(len(results["chat.search"]), 1)
        self.assertEqual(results["chat.get_context"][1]["text"], "")
        self.assertEqual(results["chat.tree_node"]["id"], "2")
        self.assertEqual(results["material.chunk"]["text"], "fixture note")
        self.assertEqual(results["preference.get"]["dailyMinutes"], 60)
        self.assertEqual(results["progress.read"][0]["minutes"], 2)
        self.assertEqual(results["material.stats"]["scope"], "account_metadata_only")

    def test_owner_deadline_and_unknown_tool_fail_before_read_dispatch(self):
        self.assert_code("denied", self.store.read, "chat.search", {}, self.ctx("8"))
        self.assert_code("timeout", self.store.read, "chat.search", {}, self.ctx(expired=True))
        self.assert_code("not_found", self.store.read, "not.a.tool", {}, self.ctx())
        self.assertEqual(self.native.calls, [])

    def test_missing_native_capabilities_never_return_synthetic_success(self):
        self.store.fleet = None
        self.assert_code("unavailable", self.store.read, "fleet.list_tasks", {"limit": 1}, self.ctx())
        self.assert_code("not_found", self.store.read, "material.chunk", {"materialId": "missing", "anchor": {"kind": "page", "offset": 1}}, self.ctx())
        self.assert_code("not_found", self.store.read, "quiz.next", {"subject": "unknown", "excludeMastered": True}, self.ctx())

    def test_three_writes_keep_atomic_receipt_replay_and_owner_scope(self):
        for tool, collection in (("memory.write", "notes"), ("progress.write", "focusSessions"), ("mistake.write", "mistakes")):
            with self.subTest(tool=tool):
                parameters = self.write_parameters(tool)
                result = self.store.execute_once(tool, parameters, self.ctx())
                revision = self.storage.revision()
                self.assertEqual(result.value["collection"], collection)
                self.assertEqual(self.store.execute_once(tool, parameters, self.ctx()).value, result.value)
                self.assertEqual(self.storage.revision(), revision)
                different = dict(parameters, idempotencyKey=parameters["idempotencyKey"])
                different["text" if tool != "progress.write" else "minutes"] = "different" if tool != "progress.write" else 16
                self.assert_code("conflict", self.store.execute_once, tool, different, self.ctx())
                self.assert_code("denied", self.store.execute_once, tool, parameters, self.ctx("8"))
        self.assertEqual(self.storage.db.execute("SELECT count(*) FROM agent_write_receipts").fetchone()[0], 3)

    def test_receipt_failure_rolls_back_document_revision_and_operation(self):
        before = self.storage.snapshot()
        operations = self.storage.db.execute("SELECT count(*) FROM operations").fetchone()[0]
        self.storage.db.execute("CREATE TRIGGER reject_receipt BEFORE INSERT ON agent_write_receipts BEGIN SELECT RAISE(ABORT, 'fixture failure'); END")
        with self.assertRaises(sqlite3.IntegrityError): self.store.execute_once("memory.write", self.write_parameters("memory.write"), self.ctx())
        after = self.storage.snapshot()
        for field in ("revision", "state", "conflictCount"):
            self.assertEqual(after[field], before[field])
        self.assertEqual(self.storage.db.execute("SELECT count(*) FROM operations").fetchone()[0], operations)
        self.assertEqual(self.storage.db.execute("SELECT count(*) FROM agent_write_receipts").fetchone()[0], 0)

    def test_concurrent_same_key_creates_only_one_record(self):
        parameters = self.write_parameters("memory.write")
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: self.store.execute_once("memory.write", parameters, self.ctx()).value, range(8)))
        self.assertTrue(all(value == results[0] for value in results))
        self.assertEqual(self.storage.db.execute("SELECT count(*) FROM agent_write_receipts").fetchone()[0], 1)
        self.assertEqual(self.storage.revision(), 1)

    def test_receipt_survives_adapter_recreation_and_undo_does_not_recreate_data(self):
        parameters = self.write_parameters("memory.write")
        result = self.store.execute_once("memory.write", parameters, self.ctx()).value
        self.store = NebulaWorkspace(self.storage, self.native, SimpleNamespace(), FleetFixture())
        self.assertEqual(self.store.execute_once("memory.write", parameters, self.ctx()).value, result)
        undo = {"tool": "memory.write", "idempotencyKey": parameters["idempotencyKey"]}
        self.assertTrue(self.store.undo_write(undo, self.ctx().deadline)["value"]["undone"])
        self.assertTrue(self.store.undo_write(undo, self.ctx().deadline)["value"]["replayed"])
        self.assertEqual(self.store.execute_once("memory.write", parameters, self.ctx()).value, result)
        self.assertFalse(any(row["id"] == result["recordId"] for row in self.storage.snapshot()["state"]["notes"]))

    def test_undo_preserves_user_edits_and_wrong_subject_writes_nothing(self):
        parameters = self.write_parameters("memory.write")
        result = self.store.execute_once("memory.write", parameters, self.ctx()).value
        self.storage.apply([{"opId": "fixture-edit", "kind": "upsert", "collection": "notes", "id": result["recordId"], "fields": {"content": "user edit"}}])
        self.assert_code("conflict", self.store.undo_write, {"tool": "memory.write", "idempotencyKey": "fixture-key"}, self.ctx().deadline)
        wrong = dict(self.write_parameters("progress.write"), subject="physics")
        before = self.storage.revision()
        self.assert_code("denied", self.store.execute_once, "progress.write", wrong, self.ctx())
        self.assertEqual(self.storage.revision(), before)

    def test_deadline_expiring_after_business_write_rolls_back_receipt_and_data(self):
        context = self.ctx()
        original = self.storage.apply
        before = self.storage.snapshot()
        def expire_after_apply(*args, **kwargs):
            result = original(*args, **kwargs)
            context.deadline = time.monotonic() - 1
            return result
        with patch.object(self.storage, "apply", side_effect=expire_after_apply):
            self.assert_code("timeout", self.store.execute_once, "memory.write", self.write_parameters("memory.write"), context)
        after = self.storage.snapshot()
        self.assertEqual((after["state"], after["revision"]), (before["state"], before["revision"]))
        self.assertEqual(self.storage.db.execute("SELECT count(*) FROM agent_write_receipts").fetchone()[0], 0)

    def test_note_search_and_subject_filter_do_not_leak_other_records(self):
        parameters = {"subject": "math", "query": "algebra", "type": "note", "limit": 1}
        result = self.store.read("material.search", parameters, self.ctx()).value
        self.assertEqual([item["materialId"] for item in result], ["note-1"])
        self.assertEqual(self.store.read("material.search", dict(parameters, subject="physics"), self.ctx()).value, [])
        self.assertEqual(self.store.read("progress.read", {"subject": "physics", "limit": 1}, self.ctx()).value, [])


if __name__ == "__main__": unittest.main()
