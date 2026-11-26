"""Single authority for Nebula control permissions, proposal decisions and receipts.

No HTTP listener, account creation, SQL tool, or ambient administrator bypass.
All callers must first pass the native account gate in AccountWorkspace.
"""
from contextlib import contextmanager, closing
import hashlib
import json
from pathlib import Path
import sqlite3
import time
from uuid import uuid4

from .runtime import ToolError, encode


class ControlStore:
    def __init__(self, database):
        self.database = Path(database)
        if self.database.is_symlink():
            raise ValueError("control ledger cannot be a symlink")
        with self.connect() as db:
            db.executescript("""
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS control_requests(
                  id TEXT PRIMARY KEY, actor TEXT NOT NULL, operation TEXT NOT NULL,
                  parameters TEXT NOT NULL, digest TEXT NOT NULL, request_key TEXT NOT NULL,
                  reason TEXT NOT NULL, expires REAL NOT NULL, created REAL NOT NULL,
                  state TEXT NOT NULL, approver TEXT, result TEXT,
                  UNIQUE(actor,request_key));
                CREATE TABLE IF NOT EXISTS control_events(
                  id INTEGER PRIMARY KEY, actor_hash TEXT NOT NULL, subject TEXT NOT NULL,
                  action TEXT NOT NULL, created REAL NOT NULL);
                CREATE TABLE IF NOT EXISTS control_proposals(
                  id TEXT PRIMARY KEY, actor TEXT NOT NULL, skill TEXT NOT NULL,
                  version INTEGER NOT NULL, parameters TEXT NOT NULL, artifact TEXT NOT NULL,
                  state TEXT NOT NULL, created REAL NOT NULL, result TEXT);
            """)
            # SupervisorLease guarantees a single owning server process. A crash
            # during execution is not evidence that the external effect failed.
            db.execute("UPDATE control_requests SET state='uncertain' WHERE state='executing'")
            db.execute("UPDATE control_proposals SET state='uncertain' WHERE state='deciding'")

    @contextmanager
    def connect(self):
        with closing(sqlite3.connect(self.database, timeout=2)) as db:
            db.row_factory = sqlite3.Row
            with db:
                yield db

    @staticmethod
    def event(db, actor, identity, action):
        db.execute("INSERT INTO control_events(actor_hash,subject,action,created) VALUES(?,?,?,?)",
                   (hashlib.sha256(actor.encode()).hexdigest(), identity, action, time.time()))

    @staticmethod
    def unpack(row):
        if row is None:
            raise ToolError("not_found")
        result = dict(row)
        for key in ("parameters", "artifact", "result"):
            if key in result and result[key] is not None:
                result[key] = json.loads(result[key])
        if result.get("state") in {"pending", "approved"} and result.get("expires", float("inf")) <= time.time():
            result["state"] = "expired"
        return result

    def request(self, actor, operation, parameters, reason, key, ttl):
        digest = hashlib.sha256(encode([operation, parameters, reason, ttl])).hexdigest()
        identity = uuid4().hex
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM control_requests WHERE actor=? AND request_key=?", (actor,key)).fetchone()
            if row:
                if row["digest"] != digest:
                    raise ToolError("conflict")
                return self.unpack(row)
            if db.execute("SELECT COUNT(*) FROM control_requests WHERE actor=? AND state IN ('pending','approved') AND expires>?", (actor,time.time())).fetchone()[0] >= 100:
                raise ToolError("unavailable")
            now = time.time()
            db.execute("INSERT INTO control_requests VALUES(?,?,?,?,?,?,?,?,?,'pending',NULL,NULL)",
                       (identity,actor,operation,encode(parameters).decode(),digest,key,reason,now+ttl,now))
            self.event(db,actor,identity,"requested")
            return self.unpack(db.execute("SELECT * FROM control_requests WHERE id=?", (identity,)).fetchone())

    def get(self, actor, identity, approver=False):
        with self.connect() as db:
            row = db.execute("SELECT * FROM control_requests WHERE id=? AND (actor=? OR ?)", (identity,actor,int(approver))).fetchone()
            return self.unpack(row)

    def requests(self, actor, approver=False):
        with self.connect() as db:
            return [self.unpack(row) for row in db.execute("SELECT * FROM control_requests WHERE actor=? OR ? ORDER BY created DESC LIMIT 50", (actor,int(approver)))]

    def decide(self, actor, identity, action, *, approver=False):
        if action not in {"approve","reject","revoke"}:
            raise ToolError("invalid")
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            item = self.unpack(db.execute("SELECT * FROM control_requests WHERE id=?", (identity,)).fetchone())
            if action in {"approve","reject"}:
                # Two explicit human identities; granting yourself capability is
                # not an approval workflow. Roles may be changed in deployment.
                if not approver or actor == item["actor"]:
                    raise ToolError("denied")
                if item["state"] != "pending":
                    raise ToolError("conflict")
                state = "approved" if action == "approve" else "rejected"
                db.execute("UPDATE control_requests SET state=?,approver=? WHERE id=?", (state,actor,identity))
            else:
                if not approver and actor != item["actor"]:
                    raise ToolError("denied")
                if item["state"] not in {"pending","approved"}:
                    raise ToolError("conflict")
                db.execute("UPDATE control_requests SET state='revoked' WHERE id=?", (identity,))
            self.event(db,actor,identity,action)
        return self.get(actor,identity,approver)

    def execute(self, actor, identity, invoke):
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            item = self.unpack(db.execute("SELECT * FROM control_requests WHERE id=? AND actor=?", (identity,actor)).fetchone())
            if item["state"] in {"succeeded","failed","submitted","uncertain"}:
                return item  # A used/uncertain permission is never consumed twice.
            if item["state"] != "approved":
                raise ToolError("denied")
            db.execute("UPDATE control_requests SET state='executing' WHERE id=?", (identity,))
            self.event(db,actor,identity,"execution_started")
        try:
            result = invoke(item["operation"],item["parameters"])
            state = "submitted" if result.get("submitted") else "succeeded" if result.get("ok") else (
                "uncertain" if result.get("error",{}).get("code") in {"timeout","unavailable"} else "failed")
        except Exception:
            state, result = "uncertain", {"ok":False,"error":{"code":"unavailable"}}
        with self.connect() as db:
            db.execute("UPDATE control_requests SET state=?,result=? WHERE id=? AND state='executing'", (state,encode(result).decode(),identity))
            self.event(db,actor,identity,"execution_"+state)
        return self.get(actor,identity)

    def reconcile_model(self, actor, identity, reader):
        item = self.get(actor,identity)
        if item["state"] != "submitted":
            return item
        operation_id = item["result"]["value"]["operationId"]
        status = reader(operation_id)
        if status.get("state") == "pending":
            return dict(item, modelOperation=status)
        state = {"completed":"succeeded","failed":"failed"}.get(status.get("state"),"uncertain")
        with self.connect() as db:
            db.execute("UPDATE control_requests SET state=?,result=? WHERE id=? AND state='submitted'", (state,encode({"ok":state=="succeeded","value":status}).decode(),identity))
            self.event(db,actor,identity,"model_"+state)
        return self.get(actor,identity)

    def stage(self, actor, skill, version, parameters, artifact):
        identity = uuid4().hex
        with self.connect() as db:
            if db.execute("SELECT COUNT(*) FROM control_proposals WHERE state='pending' AND actor=?", (actor,)).fetchone()[0] >= 100:
                raise ToolError("unavailable")
            db.execute("INSERT INTO control_proposals VALUES(?,?,?,?,?,?,'pending',?,NULL)",
                       (identity,actor,skill,version,encode(parameters).decode(),encode(artifact).decode(),time.time()))
            self.event(db,actor,identity,"proposal_staged")
        return {"proposalId":identity,"state":"pending","artifact":artifact,"requiresHumanApproval":True,"automaticDispatch":False}

    def proposals(self, actor, approver=False):
        with self.connect() as db:
            return [self.unpack(r) for r in db.execute("SELECT * FROM control_proposals WHERE actor=? OR ? ORDER BY created DESC LIMIT 50", (actor,int(approver)))]

    def decide_proposal(self, actor, identity, approve, bridge, *, approver):
        if not approver or type(approve) is not bool:
            raise ToolError("denied")
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            item = self.unpack(db.execute("SELECT * FROM control_proposals WHERE id=?", (identity,)).fetchone())
            if actor == item["actor"]:
                raise ToolError("denied")
            if item["state"] != "pending" or time.time()-item["created"] > 300:
                raise ToolError("conflict")
            db.execute("UPDATE control_proposals SET state='deciding' WHERE id=?", (identity,))
            self.event(db,actor,identity,"proposal_deciding")
        action = None
        try:
            if item["skill"] == "fleet.propose_sim_reschedule":
                if bridge is None:
                    raise ToolError("unavailable")
                action = bridge.call("approve" if approve else "reject", item["artifact"]["review"]["proposalId"],time.monotonic()+3)
            state = "approved" if approve else "rejected"
        except Exception:
            state = "uncertain"
        result = {"proposalId":identity,"state":state,"simulationAction":action,"automaticDispatch":False,
                  "artifactOnly":item["skill"] != "fleet.propose_sim_reschedule"}
        with self.connect() as db:
            db.execute("UPDATE control_proposals SET state=?,result=? WHERE id=?", (state,encode(result).decode(),identity))
            self.event(db,actor,identity,"proposal_"+state)
        return result

    def audit(self, limit=50):
        with self.connect() as db:
            return [dict(row) for row in db.execute("SELECT id,actor_hash,subject,action,created FROM control_events ORDER BY id DESC LIMIT ?", (limit,))]
