"""Adapters to existing Nebula account services. No synthetic product data."""
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from http.client import HTTPConnection, HTTPException
from http.cookies import SimpleCookie, CookieError
import hashlib
import json
import re
import time
from urllib.parse import urlencode
from uuid import uuid4

from .registry import Effect
from .runtime import ToolError, encode
from .sources import Binding, observed
from .catalog import load_tool_handlers


class NativeSession:
    """Fixed loopback authority; operation names, not a user-provided URL."""
    def __init__(self, port, cookie):
        if type(port) is not int or not 1 <= port <= 65535:
            raise ValueError("invalid native port")
        self.port = port
        try:
            parsed = SimpleCookie(cookie or "")
            entry = parsed.get("phase11_session")
        except CookieError:
            entry = None
        if not entry or not re.fullmatch(r"[A-Za-z0-9._~+=/-]{16,1024}",entry.value):
            raise ToolError("denied")
        self._cookie = "phase11_session="+entry.value
        self.access = self.call("access", {})
        if (not isinstance(self.access,dict) or not re.fullmatch(r"[1-9][0-9]{0,18}",str(self.access.get("id",""))) or
            type(self.access.get("approver")) is not bool or type(self.access.get("administrator")) is not bool or
            not isinstance(self.access.get("csrfToken"),str) or not 16 <= len(self.access["csrfToken"]) <= 512):
            raise ToolError("unavailable")
        self.actor = str(self.access["id"])

    @staticmethod
    def number(value):
        value = str(value)
        if not re.fullmatch(r"[1-9][0-9]{0,18}",value) or int(value) > 9223372036854775807:
            raise ToolError("invalid")
        return value

    def call(self, operation, p, deadline=None):
        body = None
        if operation == "access":
            path = "/api/private/agent-control/access"
        elif operation == "chat.search":
            path = "/api/search/messages/page?"+urlencode({"q":p["query"],"limit":p["limit"],
                **({"conversationId":self.number(p["conversationId"])} if p["scope"] == "conversation" else {})})
        elif operation == "chat.context":
            path = "/api/conversations/"+self.number(p["conversationId"])+"/messages/"+self.number(p["messageId"])+"/context?"+urlencode({"radius":max(1,p["radius"])})
        elif operation == "ai.nodes":
            path = "/api/admin/models?"+urlencode({"after":p.get("after","0")})
        elif operation == "ai.versions":
            path = "/api/admin/models/"+self.number(p["nodeId"])+"/versions?"+urlencode({"after":p.get("after","0")})
        elif operation == "ai.quality":
            path = "/api/admin/models/"+self.number(p["nodeId"])+"/versions/"+self.number(p["versionId"])+"/quality"
        elif operation in {"ai.model.activate","ai.model.rollback"}:
            path = "/api/admin/models/"+self.number(p["nodeId"])+"/versions/"+self.number(p["versionId"])
            body = encode({"action":operation.rsplit(".",1)[1],"reason":p["reason"]})
        elif operation == "ai.operation":
            path = "/api/admin/model-operations/"+self.number(p["operationId"])
        else:
            raise ToolError("denied")
        remaining = 3 if deadline is None else min(3,deadline-time.monotonic())
        if remaining <= 0:
            raise ToolError("timeout")
        headers = {"Cookie":self._cookie,"Accept":"application/json"}
        if body is not None:
            headers.update({"Content-Type":"application/json","X-CSRF-Token":self.access["csrfToken"]})
        connection = HTTPConnection("127.0.0.1",self.port,timeout=remaining)
        try:
            connection.request("GET" if body is None else "POST",path,body=body,headers=headers)
            response = connection.getresponse()
            raw = response.read(262145)
            if len(raw) > 262144:
                raise ToolError("invalid")
            if response.status not in {200,202}:
                raise ToolError({400:"invalid",401:"denied",403:"denied",404:"not_found",409:"conflict",429:"unavailable"}.get(response.status,"unavailable"))
            return json.loads(raw)
        except TimeoutError:
            raise ToolError("timeout") from None
        except (HTTPException,OSError,ValueError):
            raise ToolError("unavailable") from None
        finally:
            connection.close()


class NebulaWorkspace:
    SOURCE = "nebula.account.workspace"
    def __init__(self, storage, native, control, fleet=None):
        self._read_handlers, self._write_handlers = load_tool_handlers("nebula")
        self.storage,self.native,self.control,self.fleet = storage,native,control,fleet
        self.owner = native.actor
        with storage.lock:
            storage.db.execute("""CREATE TABLE IF NOT EXISTS agent_write_receipts(
                actor TEXT,tool TEXT,key TEXT,digest TEXT,result TEXT,
                collection TEXT,record_id TEXT,record_digest TEXT,
                PRIMARY KEY(actor,tool,key))""")
            storage.db.commit()

    def authorize(self, actor, name, p):
        return actor == self.owner

    @contextmanager
    def locked(self, deadline):
        if not self.storage.lock.acquire(timeout=max(0,min(2,deadline-time.monotonic()))):
            raise ToolError("timeout")
        try:
            self.storage.db.set_progress_handler(lambda: int(time.monotonic() >= deadline),1000)
            if time.monotonic() >= deadline:
                raise ToolError("timeout")
            yield self.storage.db
        finally:
            self.storage.db.set_progress_handler(None,0)
            self.storage.lock.release()

    def records(self, db, collection, condition="1", values=(), limit=50):
        # Fixed collection names, bound parameters. Only JSON metadata in this
        # account's existing document; never open/scan the 24G material library.
        if collection not in {"tasks","resources","notes","mistakes","focusSessions","flashcards","subjects"}:
            raise ToolError("denied")
        return [json.loads(row[0]) for row in db.execute(
            "SELECT j.value FROM document d,json_each(d.body,?) j WHERE "+condition+" LIMIT ?",
            ("$."+collection,*values,limit))]

    def record(self, db, identity):
        for col in ("tasks","resources","notes","mistakes","flashcards"):
            rows = self.records(db,col,"json_extract(j.value,'$.id')=?",(identity,),1)
            if rows:
                return col,rows[0]
        raise ToolError("not_found")

    @staticmethod
    def message(row):
        return {"id":str(row["id"]),"conversationId":str(row["conversationId"]),"seq":row["sequence"],
                "text":"" if row.get("state") == "recalled" else str(row.get("body",""))[:2048]}

    def read(self, name, p, ctx):
        if not self.authorize(ctx.actor_id, name, p):
            raise ToolError("denied")
        if time.monotonic() >= ctx.deadline:
            raise ToolError("timeout")
        handler = self._read_handlers.get(name)
        if handler is None:
            raise ToolError("not_found")
        return handler(self, p, ctx)

    def _write_handler(self, tool):
        handler = self._write_handlers.get(tool)
        if handler is None:
            raise ToolError("not_found")
        return handler

    def execute_once(self, tool, p, ctx):
        if not self.authorize(ctx.actor_id,tool,p):
            raise ToolError("denied")
        digest=hashlib.sha256(encode(p)).hexdigest();identity="agent-"+uuid4().hex
        with self.locked(ctx.deadline) as db:
            try:
                db.execute("BEGIN IMMEDIATE")
                receipt=db.execute("SELECT digest,result FROM agent_write_receipts WHERE actor=? AND tool=? AND key=?",(ctx.actor_id,tool,p["idempotencyKey"])).fetchone()
                if receipt:
                    if receipt[0] != digest:
                        raise ToolError("conflict")
                    db.rollback()
                    return observed(json.loads(receipt[1]),self.SOURCE)
                now=datetime.now(timezone.utc).isoformat()
                if tool in {"progress.write","mistake.write"}:
                    _,item=self.record(db,p["itemId"])
                    if item.get("subjectId",item.get("subject")) != p["subject"]:
                        raise ToolError("denied")
                col, record = self._write_handler(tool)(self, db, p, ctx, identity, now, item if tool in {"progress.write", "mistake.write"} else None)
                self.storage.apply([{"opId":uuid4().hex,"kind":"upsert","collection":col,"id":identity,"fields":record}],commit=False)
                current=self.records(db,col,"json_extract(j.value,'$.id')=?",(identity,),1)
                if not current:
                    raise ToolError("conflict")
                result={"stored":True,"recordId":identity,"collection":col,"revision":self.storage.revision()}
                db.execute("INSERT INTO agent_write_receipts VALUES(?,?,?,?,?,?,?,?)",(ctx.actor_id,tool,p["idempotencyKey"],digest,encode(result).decode(),col,identity,hashlib.sha256(encode(current[0])).hexdigest()))
                if time.monotonic() >= ctx.deadline:
                    raise ToolError("timeout")
                db.commit()
                self.storage.changed.notify_all()
            except Exception:
                db.rollback()
                raise
        return observed(result,self.SOURCE)

    def undo_write(self, p, deadline):
        # Compensation is limited to an unchanged record created by this tool.
        # Normal edits are never overwritten; receipts remain for replay safety.
        with self.locked(deadline) as db:
            try:
                db.execute("BEGIN IMMEDIATE")
                receipt=db.execute("SELECT collection,record_id,record_digest FROM agent_write_receipts WHERE actor=? AND tool=? AND key=?",(self.owner,p["tool"],p["idempotencyKey"])).fetchone()
                if not receipt:
                    raise ToolError("not_found")
                col,identity,digest=receipt
                rows=self.records(db,col,"json_extract(j.value,'$.id')=?",(identity,),1)
                if not rows:
                    db.rollback()
                    return {"ok":True,"value":{"undone":True,"recordId":identity,"replayed":True}}
                if hashlib.sha256(encode(rows[0])).hexdigest() != digest:
                    raise ToolError("conflict")
                self.storage.apply([{"opId":uuid4().hex,"kind":"delete","collection":col,"id":identity}],commit=False)
                if self.records(db,col,"json_extract(j.value,'$.id')=?",(identity,),1):
                    raise ToolError("conflict")
                if time.monotonic() >= deadline:
                    raise ToolError("timeout")
                db.commit()
                self.storage.changed.notify_all()
                return {"ok":True,"value":{"undone":True,"recordId":identity}}
            except Exception:
                db.rollback()
                raise

    def stage(self, actor, skill, version, parameters, artifact):
        if actor != self.owner:
            raise ToolError("denied")
        return self.control.stage(actor,skill,version,parameters,artifact)


def bind_nebula(runtime,store):
    for name,spec in runtime.registry.tools.items():
        source = "nebula.chat.authorized" if name.startswith("chat.") else "workflow.checkpoint.projection" if name == "workflow.get_run" else "fleet.grpc.live" if name.startswith("fleet.") or name in {"evidence.get","metrics.get"} else store.SOURCE
        if spec.effect == Effect.READ:
            runtime.bind(name,Binding(source,store.authorize,read=lambda p,c,n=name:store.read(n,p,c)))
        else:
            runtime.bind(name,Binding(source,store.authorize,write=store))
