import json
import sqlite3
import tempfile
import time
import unittest
from pathlib import Path
import sys
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT))
from backend.agent.tool_runtime.control import ControlStore
from backend.agent.tool_runtime.registry import default_registry,Effect
from backend.agent.tool_runtime.schema import validate

class ControlLedger(unittest.TestCase):
    def setUp(self):
        self.dir=Path(tempfile.mkdtemp(prefix="control-ledger-",dir=ROOT/"build-function3"))
        self.ledger=ControlStore(self.dir/"agent-control.sqlite3")
    def test_idempotency_expiry_approval_revoke_and_crash_recovery(self):
        a=self.ledger.request("7","study.log_progress",{"x":1},"reason","same",60)
        self.assertEqual(self.ledger.request("7","study.log_progress",{"x":1},"reason","same",60)["id"],a["id"])
        with self.assertRaises(Exception): self.ledger.request("7","study.log_progress",{"x":2},"reason","same",60)
        with self.assertRaises(Exception): self.ledger.decide("7",a["id"],"approve",approver=True)
        self.assertEqual(self.ledger.decide("9",a["id"],"approve",approver=True)["state"],"approved")
        calls=[]
        result=self.ledger.execute("7",a["id"],lambda op,p:calls.append(1) or {"ok":True,"value":{"done":True}})
        self.assertEqual(result["state"],"succeeded");self.assertEqual(len(calls),1)
        self.assertEqual(self.ledger.execute("7",a["id"],lambda op,p:calls.append(1) or {"ok":True})["state"],"succeeded")
        self.assertEqual(len(calls),1)
        b=self.ledger.request("7","study.log_progress",{"x":3},"reason","other",60)
        with self.ledger.connect() as db: db.execute("UPDATE control_requests SET state='executing' WHERE id=?",(b["id"],))
        ControlStore(self.dir/"agent-control.sqlite3")
        self.assertEqual(self.ledger.get("7",b["id"])["state"],"uncertain")
        with self.assertRaises(Exception): self.ledger.decide("7",b["id"],"revoke")
    def test_catalog_is_strict_and_versions_are_addressable(self):
        r=default_registry();self.assertEqual(len(r.skills),19)
        s=r.skill("fleet.get_task_status");self.assertIsNotNone(r.skill(s.name,1))
        with self.assertRaises(KeyError):r.skill(s.name,2)
        self.assertTrue(all(spec.parameters.get("additionalProperties") is False for spec in r.skills.values()))
        self.assertTrue(all(spec.timeout_ms != 5000 for spec in r.skills.values()))

if __name__ == '__main__':unittest.main()
