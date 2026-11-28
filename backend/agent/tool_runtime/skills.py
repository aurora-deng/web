"""Executable skill recipes, deterministic Planner, and non-model confirmation grants.

FleetAgent remains a deliberately narrow C++ proposal gate. This is the general
read/proposal/external dispatcher; effect checks are NOT removed from FleetAgent.
"""
from dataclasses import dataclass
from datetime import date, datetime, timezone
import hashlib
import json
import re
import subprocess
import time
from uuid import uuid4

from .registry import Effect, Precondition
from .runtime import AuditRecord, ToolError, encode, planner_mode
from .schema import SchemaError, validate
from .workspace import connect


@dataclass(frozen=True)
class ConfirmedIntent:
    """Trusted UI/controller capability. Never construct this from a model-supplied JSON field."""
    actor: str
    skill: str
    version: int
    digest: str
    expires: float
    issuer: object


class RulesPlanner:
    """Production deterministic routing. No test double, model download or hidden network call."""
    def __init__(self, registry): self.registry=registry
    def plan(self, intent, parameters):
        routes={"status":"fleet.get_task_status","recall":"chat.recall","summarize":"chat.summarize",
                "search_material":"study.search_material","log_progress":"study.log_progress","mistake_add":"study.mistake_add"}
        name=routes.get(intent,intent)
        try: spec=self.registry.skill(name)
        except KeyError: raise ToolError("not_found") from None
        validate(spec.parameters,parameters)
        return {"skillId":name,"version":spec.version,"parameters":parameters,"planner":"rules"}


class FleetSimulationBridge:
    """Configured local executable + loopback address; no shell, arbitrary URL, or credential output."""
    def __init__(self, executable, address, environment):
        from pathlib import Path
        self.executable=str(Path(executable).resolve(strict=True))
        if re.fullmatch(r"127\.0\.0\.1:[0-9]{4,5}",address) is None or not 1024<=int(address.split(":")[1])<=65535:
            raise ValueError("loopback fleet address required")
        self.address=address;self.environment=environment
    def call(self, action, identity, deadline):
        remaining=deadline-time.monotonic()
        if remaining<=0: raise ToolError("timeout")
        try:
            result=subprocess.run([self.executable,self.address,action,identity],capture_output=True,
                                  timeout=remaining,env=self.environment,check=False)
        except subprocess.TimeoutExpired: raise ToolError("timeout") from None
        if len(result.stdout)>16384: raise ToolError("invalid")
        try: value=json.loads(result.stdout)
        except ValueError: raise ToolError("unavailable") from None
        if result.returncode or value.get("state") not in {"pending","approved","rejected"}: raise ToolError("denied")
        return value


class SkillRuntime:
    def __init__(self, tools, store, fleet_bridge=None):
        self.tools=tools; self.registry=tools.registry; self.store=store; self.fleet_bridge=fleet_bridge
        self.planner=RulesPlanner(self.registry); self._issuer=object()
        from .catalog import load_recipes
        self.recipes=load_recipes(self)
        if set(self.recipes)!=set(self.registry.skills): raise ValueError("every advertised skill must have exactly one executable recipe")

    def confirm(self, actor, name, parameters, version=1):
        """Called only by an authenticated, CSRF-checked user action, not the Planner/tool interface."""
        spec=self.registry.skill(name,version); validate(spec.parameters,parameters)
        if spec.effect!=Effect.EXTERNAL or not self.store.authorize(actor,name,parameters): raise ToolError("denied")
        return ConfirmedIntent(actor,name,version,hashlib.sha256(encode(parameters)).hexdigest(),time.monotonic()+60,self._issuer)

    def invoke(self, name, parameters, *, actor_id, version=1, confirmation=None, trace_id=None):
        started=time.monotonic();observations=[];trace_id=trace_id or uuid4().hex
        def audit(code):
            safe_name=name if isinstance(name,str) and re.fullmatch(r"[a-z0-9_.]{1,96}",name) else "invalid"
            safe_trace=trace_id if isinstance(trace_id,str) and re.fullmatch(r"[A-Za-z0-9_-]{1,64}",trace_id) else "invalid"
            safe_actor=actor_id if isinstance(actor_id,str) and re.fullmatch(r"[A-Za-z0-9_-]{1,64}",actor_id) else "invalid"
            summary={k:type(v).__name__ for k,v in parameters.items()} if isinstance(parameters,dict) and len(parameters)<=32 and all(isinstance(k,str) and len(k)<=64 for k in parameters) else {}
            self.tools.audit.append(AuditRecord(safe_name,safe_trace,hashlib.sha256(safe_actor.encode()).hexdigest(),summary,round((time.monotonic()-started)*1000,3),code,datetime.now(timezone.utc).isoformat()))
        try:
            if type(version) is not int or not isinstance(name,str): raise ToolError("invalid")
            try: spec=self.registry.skill(name,version)
            except KeyError: raise ToolError("not_found") from None
            validate(spec.parameters,parameters)
            if name=="chat.recall" and ((parameters["scope"]=="conversation")!=("conversationId" in parameters)): raise ToolError("invalid")
            if "date" in parameters:
                try: date.fromisoformat(parameters["date"])
                except ValueError: raise ToolError("invalid") from None
            if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}",actor_id) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}",trace_id): raise ToolError("invalid")
            if not self.store.authorize(actor_id,name,parameters): raise ToolError("denied")
            if Precondition.CONFIRMED_USER_INTENT in spec.preconditions:
                if (not isinstance(confirmation,ConfirmedIntent) or confirmation.issuer is not self._issuer or
                    (confirmation.actor,confirmation.skill,confirmation.version,confirmation.digest)!=(actor_id,name,version,hashlib.sha256(encode(parameters)).hexdigest()) or confirmation.expires<=time.monotonic()):
                    raise ToolError("denied")
            if Precondition.ATOMIC_IDEMPOTENT_PORT in spec.preconditions:
                port="progress.write" if name=="study.log_progress" else "mistake.write"
                binding=self.tools._bindings.get(port)
                if binding is None or not callable(getattr(binding.write,"execute_once",None)): raise ToolError("denied")
            audit("started")
            deadline=started+spec.timeout_ms/1000
            def call(tool,args):
                observation=self.tools.call(tool,args,actor_id=actor_id,trace_id=trace_id,deadline=deadline)
                observations.append(observation)
                if not observation["ok"]: raise ToolError(observation["error"]["code"])
                return observation["value"]
            value=self.recipes[name](parameters,call,actor_id,deadline)
            if time.monotonic()>=deadline: raise ToolError("timeout")
            result={"ok":True,"skillId":name,"version":version,"value":value,"source":"skill.rules",
                    "readAt":datetime.now(timezone.utc).isoformat(),"fresh":all(r["fresh"] for r in observations),
                    "truncated":any(r["truncated"] for r in observations),"planner": "rules",
                    "evidenceSources":sorted({r["source"] for r in observations})}
            if len(encode(result))>32768: raise ToolError("invalid")
            audit("ok");return result
        except Exception as error:
            code=error.code if isinstance(error,ToolError) else "invalid" if isinstance(error,(SchemaError,TypeError,ValueError)) else "unavailable"
            try: audit(code)
            except Exception: code="unavailable"
            return {"ok":False,"error":{"code":code},"source":"skill.runtime","readAt":datetime.now(timezone.utc).isoformat(),"fresh":False,"truncated":False,"value":None}

    def _stage(self,name,p,artifact,actor): return self.store.stage(actor,name,1,p,artifact)

    def decide(self,identity,*,actor_id,approve):
        """Human decision only. Non-simulation proposals remain artifacts, not hidden dispatches."""
        if type(approve) is not bool: raise ToolError("invalid")
        item=self.store.proposal(actor_id,identity)
        expected="approved" if approve else "rejected"
        if item["state"]==expected: return {"proposalId":identity,"state":expected,"replayed":True}
        if item["state"]!="pending": raise ToolError("conflict")
        if time.time()-item["created"]>300: raise ToolError("denied")
        action=None
        if item["skill"]=="fleet.propose_sim_reschedule":
            if self.fleet_bridge is None: raise ToolError("unavailable")
            action=self.fleet_bridge.call("approve" if approve else "reject",item["artifact"]["review"]["proposalId"],time.monotonic()+3)
        with connect(self.store.database) as db:
            changed=db.execute("UPDATE proposals SET state=? WHERE actor=? AND id=? AND state='pending'",(expected,actor_id,identity)).rowcount
            if not changed: raise ToolError("conflict")
        return {"proposalId":identity,"state":expected,"simulationAction":action,"automaticDispatch":False}
