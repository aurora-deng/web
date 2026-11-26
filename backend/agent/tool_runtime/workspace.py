"""Owner-scoped local adapters. Only explicit deployment paths; no generic SQL/file tool.

The isolated preview stores real records in SQLite. Synthetic fixtures are opt-in.
Fleet/workflow snapshots open configured databases read-only, never production paths.
"""
from contextlib import contextmanager, closing
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sqlite3
import time
from uuid import uuid4

from .runtime import ToolError, encode
from .sources import Binding, observed


@contextmanager
def connect(path, readonly=False):
    uri=Path(path).resolve().as_uri()+"?mode=ro" if readonly else str(path)
    with closing(sqlite3.connect(uri,uri=readonly,timeout=1)) as db:
        db.row_factory=sqlite3.Row
        db.execute("PRAGMA foreign_keys=ON")
        if readonly: db.execute("PRAGMA query_only=ON")
        with db: yield db


class FleetSnapshot:
    """Bounded local read model; unlike C++ gRPC Status, this snapshot has no live connection flag."""
    def __init__(self, ledger, journal):
        self.ledger=Path(ledger).resolve()
        self.journal=Path(journal).resolve()
        for p in (self.ledger,self.journal):
            if not p.is_file() or p.is_symlink(): raise ValueError("snapshot must be an existing private regular file")

    @staticmethod
    def task(row):
        phases={1:"queued",2:"leased",3:"accepted",4:"executing",5:"succeeded",6:"failed",7:"uncertain"}
        return {"taskId":row["task_id"],"traceId":row["trace_id"],"epoch":row["epoch"],
                "robotId":row["robot_id"],"phase":phases[row["phase"]],"outcome":row["outcome"],
                "needsReconciliation":row["phase"]==7,"connectionState":"not_observed"}

    def status(self, task_id):
        with connect(self.ledger,True) as db:
            row=db.execute("SELECT task_id,trace_id,epoch,robot_id,phase,outcome FROM tasks WHERE task_id=?",(task_id,)).fetchone()
            if row is None: raise ToolError("not_found")
            return self.task(row)

    def tasks(self, p):
        with connect(self.ledger,True) as db:
            rows=db.execute("SELECT task_id,trace_id,epoch,robot_id,phase,outcome FROM tasks WHERE task_id>? ORDER BY task_id LIMIT ?",
                            (p.get("cursor",""),p["limit"])).fetchall()
            return [self.task(row) for row in rows]

    def trace(self,p):
        with connect(self.journal,True) as db:
            if db.execute("SELECT 1 FROM workflow_runs WHERE run_id=?",(p["runId"],)).fetchone() is None: raise ToolError("not_found")
            return [dict(row) for row in db.execute("SELECT node_id,attempts,effect,result_kind,error_code,substr(value,1,4096) AS value FROM workflow_nodes WHERE run_id=? ORDER BY node_id LIMIT ?",(p["runId"],p["nodeLimit"]))]

    def counts(self):
        with connect(self.ledger,True) as db:
            active=db.execute("SELECT COUNT(*) FROM tasks WHERE phase IN (2,3,4,7)").fetchone()[0]
        with connect(self.journal,True) as db:
            failed=db.execute("SELECT COUNT(DISTINCT run_id) FROM workflow_nodes WHERE result_kind=2").fetchone()[0]
        return {"fleet.active_tasks":active,"workflow.failed_runs":failed}


class NativeFleetPort:
    """Live, authorized gRPC via the built CLI; checkpoint JSON is exported by its owning Edge."""
    def __init__(self,ctl,address,environment,checkpoint):
        import re
        self.ctl=str(Path(ctl).resolve(strict=True));self.address=address;self.environment=environment
        if re.fullmatch(r"127\.0\.0\.1:[0-9]{4,5}",address) is None:raise ValueError("loopback required")
        self.checkpoint=Path(checkpoint).resolve(strict=True)
    def _call(self,args,deadline=None):
        import subprocess
        try:
            result=subprocess.run([self.ctl,self.address,*args],capture_output=True,env=self.environment,
                                  timeout=max(0.001,min(2,(deadline-time.monotonic()) if deadline else 2)),check=False)
        except subprocess.TimeoutExpired:raise ToolError("timeout") from None
        if len(result.stdout)>32768:raise ToolError("invalid")
        if result.returncode:
            code="denied" if b'rpc_status=16' in result.stderr or b'rpc_status=7' in result.stderr else "not_found" if b'rpc_status=5' in result.stderr else "unavailable"
            raise ToolError(code)
        try:return json.loads(result.stdout)
        except ValueError:raise ToolError("invalid") from None
    def status(self,identity,deadline=None):return self._call(["status",identity],deadline)
    def tasks(self,p,deadline=None):return self._call(["list",str(p["limit"]),*([p["cursor"]] if p.get("cursor") else [])],deadline)
    def trace(self,p):
        if self.checkpoint.is_symlink() or self.checkpoint.stat().st_size>2*1024*1024:raise ToolError("invalid")
        data=json.loads(self.checkpoint.read_bytes())
        if (type(data) is not dict or data.get("format") != 1 or type(data.get("nodes")) is not list
                or len(data["nodes"]) > 8192 or type(data.get("truncated",False)) is not bool):
            raise ToolError("invalid")
        runs=data.get("runs")
        if runs is not None and (type(runs) is not list or len(runs)>32 or any(type(run) is not str for run in runs)):
            raise ToolError("invalid")
        if any(type(row) is not dict or type(row.get("run_id")) is not str for row in data["nodes"]):
            raise ToolError("invalid")
        rows=[r for r in data["nodes"] if r["run_id"]==p["runId"]]
        if not rows and (runs is None or p["runId"] not in runs):
            # A bounded hot view cannot prove that an older run never existed.
            raise ToolError("unavailable" if data.get("truncated",False) else "not_found")
        return [{k:v for k,v in row.items() if k!="run_id"} for row in rows[:p["nodeLimit"]]]
    def counts(self):
        load=self._call(["load"])
        return {"fleet.active_tasks":load["active"]}


class WorkspaceStore:
    SOURCE="workspace.sqlite"
    def __init__(self, database, owner, fleet=None):
        self.database=Path(database).resolve(); self.owner=owner; self.fleet=fleet
        if self.database.is_symlink(): raise ValueError("invalid workspace database")
        with connect(self.database) as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS messages(actor TEXT,conversation TEXT,id TEXT,seq INTEGER,text TEXT,parent TEXT,PRIMARY KEY(actor,id),UNIQUE(actor,conversation,seq));
                CREATE TABLE IF NOT EXISTS memories(actor TEXT,id TEXT,text TEXT,PRIMARY KEY(actor,id));
                CREATE TABLE IF NOT EXISTS preferences(actor TEXT PRIMARY KEY,value TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS materials(actor TEXT,id TEXT,title TEXT,subject TEXT,kind TEXT,PRIMARY KEY(actor,id));
                CREATE TABLE IF NOT EXISTS chunks(actor TEXT,material TEXT,kind TEXT,offset INTEGER,text TEXT,PRIMARY KEY(actor,material,kind,offset));
                CREATE TABLE IF NOT EXISTS questions(actor TEXT,id TEXT,subject TEXT,question TEXT,answer TEXT,mastered INTEGER,PRIMARY KEY(actor,id));
                CREATE TABLE IF NOT EXISTS progress(actor TEXT,id TEXT,date TEXT,subject TEXT,item TEXT,minutes INTEGER,PRIMARY KEY(actor,id));
                CREATE TABLE IF NOT EXISTS mistakes(actor TEXT,id TEXT,subject TEXT,item TEXT,text TEXT,PRIMARY KEY(actor,id));
                CREATE TABLE IF NOT EXISTS receipts(actor TEXT,tool TEXT,key TEXT,digest TEXT,result TEXT,PRIMARY KEY(actor,tool,key));
                CREATE TABLE IF NOT EXISTS proposals(actor TEXT,id TEXT,skill TEXT,version INTEGER,parameters TEXT,artifact TEXT,state TEXT,created REAL,PRIMARY KEY(actor,id));
                CREATE TABLE IF NOT EXISTS metric_samples(name TEXT,at REAL,value REAL);
                CREATE INDEX IF NOT EXISTS metric_time ON metric_samples(name,at);
            """)

    def authorize(self,actor,name,parameters): return actor==self.owner

    def seed_demo(self):
        """Only call for an explicitly requested, empty preview workspace. Never import personal data."""
        with connect(self.database) as db:
            if any(db.execute("SELECT 1 FROM "+t+" LIMIT 1").fetchone() for t in ("messages","materials","questions","progress","memories","mistakes","proposals")):
                raise ValueError("demo seed requires empty workspace")
            db.executemany("INSERT INTO messages VALUES (?,?,?,?,?,?)",[
                (self.owner,"demo-chat","m1",1,"What makes a workflow crash-safe?",None),
                (self.owner,"demo-chat","m2",2,"Persist checkpoints and reconcile unknown external outcomes.","m1"),
                (self.owner,"demo-chat","m3",3,"An idempotency receipt must commit with its business effect.","m1")])
            db.execute("INSERT INTO materials VALUES (?,?,?,?,?)",(self.owner,"demo-note","Durable workflow notes","systems","note"))
            db.execute("INSERT INTO chunks VALUES (?,?,?,?,?)",(self.owner,"demo-note","page",1,"Checkpoints persist completed nodes. External effects with unknown outcomes require reconciliation, not blind retry."))
            db.execute("INSERT INTO questions VALUES (?,?,?,?,?,?)",(self.owner,"q1","systems","Why must an unknown actuator outcome not be retried blindly?","It may already have executed; reconcile first.",0))
            db.execute("INSERT INTO preferences VALUES (?,?)",(self.owner,encode({"dailyMinutes":60,"syntheticDemo":True}).decode()))

    def _context(self,db,p,actor):
        anchor=db.execute("SELECT seq FROM messages WHERE actor=? AND conversation=? AND id=?",(actor,p["conversationId"],p["messageId"])).fetchone()
        if not anchor: raise ToolError("not_found")
        return [dict(r) for r in db.execute("SELECT id,seq,substr(text,1,2048) AS text,parent FROM messages WHERE actor=? AND conversation=? AND seq BETWEEN ? AND ? ORDER BY seq LIMIT 41",
               (actor,p["conversationId"],anchor[0]-p["radius"],anchor[0]+p["radius"]))]

    def read(self,name,p,ctx):
        if time.monotonic()>=ctx.deadline: raise ToolError("timeout")
        if ctx.actor_id!=self.owner: raise ToolError("denied")
        actor=ctx.actor_id
        if name.startswith("fleet.") or name in {"evidence.get","workflow.get_run"}:
            if self.fleet is None: raise ToolError("unavailable")
            if name=="fleet.list_tasks": value=self.fleet.tasks(p,ctx.deadline) if isinstance(self.fleet,NativeFleetPort) else self.fleet.tasks(p)
            elif name=="workflow.get_run": value=self.fleet.trace(p)
            else: value=self.fleet.status(p.get("taskId",p.get("evidenceId")),ctx.deadline) if isinstance(self.fleet,NativeFleetPort) else self.fleet.status(p.get("taskId",p.get("evidenceId")))
            source="workflow.checkpoint.projection" if name=="workflow.get_run" and isinstance(self.fleet,NativeFleetPort) else "workflow.sqlite.checkpoint" if name=="workflow.get_run" else "fleet.grpc.live" if isinstance(self.fleet,NativeFleetPort) else "fleet.sqlite.snapshot"
            stamp=observed(value,source)
            if name=="workflow.get_run" and isinstance(self.fleet,NativeFleetPort):
                from .sources import SourceValue
                stamp=SourceValue(value,source,datetime.fromtimestamp(self.fleet.checkpoint.stat().st_mtime,timezone.utc),True)
            return stamp
        with connect(self.database) as db:
            if name=="chat.search":
                clause=" AND conversation=?" if p["scope"]=="conversation" else ""
                values=[actor,p["query"]]+([p["conversationId"]] if clause else [])+[p["limit"]]
                value=[dict(r) for r in db.execute("SELECT id,conversation,seq,substr(text,1,2048) AS text FROM messages WHERE actor=? AND instr(lower(text),lower(?))>0"+clause+" ORDER BY conversation,seq LIMIT ?",values)]
            elif name=="chat.get_context": value=self._context(db,p,actor)
            elif name=="chat.tree_node":
                row=db.execute("SELECT id,parent,substr(text,1,2048) AS text FROM messages WHERE actor=? AND conversation=? AND id=?",(actor,p["conversationId"],p["nodeId"])).fetchone()
                if not row: raise ToolError("not_found")
                value=dict(row)
            elif name=="memory.read": value=[dict(r) for r in db.execute("SELECT id,text FROM memories WHERE actor=? ORDER BY id LIMIT ?",(actor,p["limit"]))]
            elif name=="preference.get":
                row=db.execute("SELECT value FROM preferences WHERE actor=?",(actor,)).fetchone();value=json.loads(row[0]) if row else {}
            elif name=="material.search":
                value=[{"materialId":r["id"],"title":r["title"],"kind":r["kind"]} for r in db.execute("SELECT id,substr(title,1,256) AS title,kind FROM materials WHERE actor=? AND subject=? AND kind=? AND instr(lower(title),lower(?))>0 ORDER BY id LIMIT ?",(actor,p["subject"],p["type"],p["query"],p["limit"]))]
            elif name=="material.chunk":
                a=p["anchor"];row=db.execute("SELECT text FROM chunks WHERE actor=? AND material=? AND kind=? AND offset=?",(actor,p["materialId"],a["kind"],a["offset"])).fetchone()
                if not row: raise ToolError("not_found")
                value={"materialId":p["materialId"],"anchor":a,"text":row[0]}
            elif name=="material.stats":
                value={"count":db.execute("SELECT COUNT(*) FROM materials WHERE actor=?",(actor,)).fetchone()[0],"scope":"actor_index_only"}
            elif name=="quiz.next":
                row=db.execute("SELECT id,question,answer FROM questions WHERE actor=? AND subject=? AND mastered=0 ORDER BY id LIMIT 1",(actor,p["subject"])).fetchone()
                if not row: raise ToolError("not_found")
                value=dict(row)
            elif name=="progress.read": value=[dict(r) for r in db.execute("SELECT id,date,item,minutes FROM progress WHERE actor=? AND subject=? ORDER BY date DESC,id LIMIT ?",(actor,p["subject"],p["limit"]))]
            elif name=="metrics.get":
                now=time.time()
                if self.fleet:
                    for metric,number in self.fleet.counts().items(): db.execute("INSERT INTO metric_samples VALUES (?,?,?)",(metric,now,number))
                db.execute("DELETE FROM metric_samples WHERE at<?",(now-86400,))
                db.execute("DELETE FROM metric_samples WHERE rowid NOT IN (SELECT rowid FROM metric_samples ORDER BY at DESC LIMIT 10000)")
                value=[]
                for metric in p["names"]:
                    rows=db.execute("SELECT at,value FROM metric_samples WHERE name=? AND at>=? ORDER BY at DESC LIMIT 64",(metric,now-p["windowSeconds"])).fetchall()
                    if not rows: raise ToolError("unavailable")
                    value.extend({"name":metric,"sampledAt":datetime.fromtimestamp(r[0],timezone.utc).isoformat(),"value":r[1]} for r in rows)
            else: raise ToolError("not_found")
        return observed(value,self.SOURCE)

    def execute_once(self,tool,p,ctx):
        if ctx.actor_id!=self.owner: raise ToolError("denied")
        if time.monotonic()>=ctx.deadline: raise ToolError("timeout")
        digest=hashlib.sha256(encode(p)).hexdigest();key=p["idempotencyKey"]
        with connect(self.database) as db:
            db.execute("BEGIN IMMEDIATE")
            row=db.execute("SELECT digest,result FROM receipts WHERE actor=? AND tool=? AND key=?",(ctx.actor_id,tool,key)).fetchone()
            if row:
                if row[0]!=digest: raise ToolError("conflict")
                return observed(json.loads(row[1]),self.SOURCE)
            if db.execute("SELECT COUNT(*) FROM receipts").fetchone()[0]>=10000: raise ToolError("unavailable")
            identity=uuid4().hex
            if tool=="memory.write": db.execute("INSERT INTO memories VALUES (?,?,?)",(ctx.actor_id,identity,p["text"]))
            elif tool=="progress.write": db.execute("INSERT INTO progress VALUES (?,?,?,?,?,?)",(ctx.actor_id,identity,p["date"],p["subject"],p["itemId"],p["minutes"]))
            elif tool=="mistake.write": db.execute("INSERT INTO mistakes VALUES (?,?,?,?,?)",(ctx.actor_id,identity,p["subject"],p["itemId"],p["text"]))
            else: raise ToolError("not_found")
            result={"stored":True,"recordId":identity}
            db.execute("INSERT INTO receipts VALUES (?,?,?,?,?)",(ctx.actor_id,tool,key,digest,encode(result).decode()))
            if time.monotonic()>=ctx.deadline: raise ToolError("timeout") # rollback before commit
        return observed(result,self.SOURCE)

    def stage(self,actor,skill,version,parameters,artifact):
        if actor!=self.owner: raise ToolError("denied")
        identity=uuid4().hex
        with connect(self.database) as db:
            if db.execute("SELECT COUNT(*) FROM proposals").fetchone()[0]>=1000: raise ToolError("unavailable")
            db.execute("INSERT INTO proposals VALUES (?,?,?,?,?,?,?,?)",(actor,identity,skill,version,encode(parameters).decode(),encode(artifact).decode(),"pending",time.time()))
        return {"proposalId":identity,"state":"pending","artifact":artifact,"requiresHumanApproval":True,"automaticDispatch":False}

    def proposal(self,actor,identity):
        if actor!=self.owner: raise ToolError("denied")
        with connect(self.database,True) as db:
            row=db.execute("SELECT * FROM proposals WHERE actor=? AND id=?",(actor,identity)).fetchone()
            if not row: raise ToolError("not_found")
            value=dict(row);value["parameters"]=json.loads(value["parameters"]);value["artifact"]=json.loads(value["artifact"])
            return value


def bind_workspace(runtime,store):
    for name,spec in runtime.registry.tools.items():
        native=isinstance(store.fleet,NativeFleetPort)
        source=("workflow.checkpoint.projection" if native else "workflow.sqlite.checkpoint") if name=="workflow.get_run" else ("fleet.grpc.live" if native else "fleet.sqlite.snapshot") if name.startswith("fleet.") or name=="evidence.get" else store.SOURCE
        if spec.effect.value=="read":
            runtime.bind(name,Binding(source,store.authorize,read=lambda p,c,n=name:store.read(n,p,c)))
        else: runtime.bind(name,Binding(source,store.authorize,write=store))
