"""Private control routes hosted inside the existing Nebula account component."""
from dataclasses import asdict
import json
import re
import secrets
import time

from backend.agent.tool_runtime.nebula import NativeSession, NebulaWorkspace, bind_nebula
from backend.agent.tool_runtime.registry import Effect, default_registry, obj, text, ID, KEY, integer
from backend.agent.tool_runtime.runtime import ToolRuntime, ToolError, SqliteAuditSink, encode
from backend.agent.tool_runtime.skills import SkillRuntime
from backend.agent.tool_runtime.schema import SchemaError, validate

PREFIX = "/api/private/agent-control/"
REGISTRY = default_registry()
AI_WRITE = obj({"nodeId":ID,"versionId":ID,"reason":text(512)})
UNDO = obj({"tool":text(32,enum=["progress.write","mistake.write","memory.write"]),"idempotencyKey":KEY})
EXTRA = {"ai.model.activate":AI_WRITE,"ai.model.rollback":AI_WRITE,"study.undo_write":UNDO}


def validate_operation(operation, parameters, native):
    if operation in EXTRA:
        if operation.startswith("ai.") and not native.access["administrator"]:
            raise ToolError("denied")
        validate(EXTRA[operation],parameters)
        if operation.startswith("ai."):
            native.number(parameters["nodeId"]);native.number(parameters["versionId"])
    else:
        spec=REGISTRY.skill(operation)
        if spec.effect != Effect.EXTERNAL:
            raise ToolError("denied")
        validate(spec.parameters,parameters)


def packet(schema, body):
    # The packet carries an opaque parameters object only until the selected
    # operation's strict schema is known. It is validated immediately afterwards.
    if "parameters" in schema["properties"]:
        if type(body) is not dict or type(body.get("parameters")) is not dict:
            raise ToolError("invalid")
        fields=dict(schema["properties"]);fields.pop("parameters")
        validate(obj(fields),{k:v for k,v in body.items() if k != "parameters"})
    else:
        validate(schema,body)


def strict_object(pairs):
    result={}
    for key,value in pairs:
        if key in result:
            raise ValueError("duplicate key")
        result[key]=value
    return result


def dispatch(handler, args, target, accounts, ledger, fleet, bridge, origins, origin_key):
    """Returns True for handled control paths; no extra listener or identity."""
    if not target.path.startswith(PREFIX) or target.path == PREFIX+"access":
        return False
    try:
        if ledger is None:
            raise ToolError("not_found")
        if target.query or target.fragment or target.netloc or target.scheme:
            raise ToolError("invalid")
        native=NativeSession(args.auth_port,handler.headers.get("Cookie"))
        actor=native.actor
        changed=handler.command == "POST"
        if handler.command not in {"GET","POST"}:
            raise ToolError("not_found")
        body={}
        if changed:
            # Every route that can write, including plan/invoke, must be reached
            # from an approved browser origin with the native session's CSRF.
            if (origin_key(handler.headers.get("Origin","")) not in origins or
                not secrets.compare_digest(native.access["csrfToken"].encode(),handler.headers.get("X-CSRF-Token","").encode())):
                raise ToolError("denied")
            lengths=handler.headers.get_all("Content-Length",[])
            if (handler.headers.get("Transfer-Encoding") or len(lengths)!=1 or
                not re.fullmatch(r"[0-9]{1,5}",lengths[0]) or not 1 <= int(lengths[0]) <= 32768 or
                handler.headers.get("Content-Type","").split(";",1)[0] != "application/json"):
                raise ToolError("invalid")
            body=json.loads(handler.rfile.read(int(lengths[0])),object_pairs_hook=strict_object,
                            parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
            if type(body) is not dict:
                raise ToolError("invalid")
        path=target.path[len(PREFIX):]
        if path == "catalog" and not changed:
            skills=[]
            for spec in REGISTRY.skills.values():
                value=asdict(spec)
                value["effect"]=spec.effect.value
                value["preconditions"]=[item.value for item in spec.preconditions]
                value["timeoutMs"]=value.pop("timeout_ms")
                value["availability"]="configured" if not spec.name.startswith(("fleet.","workflow.","metrics.")) or fleet else "unavailable"
                skills.append(value)
            return reply(handler,{"skills":skills,"operations":EXTRA,"planner":"rules","simulationOnly":True,"fleetConfigured":fleet is not None})
        if path == "requests" and not changed:
            return reply(handler,{"requests":ledger.requests(actor,native.access["approver"])})
        if path == "proposals" and not changed:
            return reply(handler,{"proposals":ledger.proposals(actor,native.access["approver"])})
        if path == "audit" and not changed:
            if not native.access["approver"]:
                raise ToolError("denied")
            return reply(handler,{"events":ledger.audit()})
        if path == "ai" and not changed:
            return reply(handler,{"nodes":native.call("ai.nodes",{})})
        if path == "ai/read" and changed:
            packet(obj({"operation":text(24,enum=["ai.versions","ai.quality","ai.operation"]),"parameters":{"type":"object"}}),body)
            schemas={"ai.versions":obj({"nodeId":ID}),"ai.quality":obj({"nodeId":ID,"versionId":ID}),"ai.operation":obj({"operationId":ID})}
            validate(schemas[body["operation"]],body["parameters"])
            return reply(handler,{"value":native.call(body["operation"],body["parameters"])})
        if path == "requests" and changed:
            packet(obj({"operation":text(96),"parameters":{"type":"object"},"reason":text(512),"idempotencyKey":KEY,"expiresSeconds":integer(60,3600)}),body)
            validate_operation(body["operation"],body["parameters"],native)
            return reply(handler,ledger.request(actor,body["operation"],body["parameters"],body["reason"],body["idempotencyKey"],body["expiresSeconds"]))
        if path == "decide" and changed:
            packet(obj({"requestId":ID,"action":text(8,enum=["approve","reject","revoke"])}),body)
            return reply(handler,ledger.decide(actor,body["requestId"],body["action"],approver=native.access["approver"]))
        if path == "proposal/decide" and changed:
            packet(obj({"proposalId":ID,"approve":{"type":"boolean"}}),body)
            return reply(handler,ledger.decide_proposal(actor,body["proposalId"],body["approve"],bridge,approver=native.access["approver"]))
        if path == "reconcile" and changed:
            packet(obj({"requestId":ID}),body)
            return reply(handler,ledger.reconcile_model(actor,body["requestId"],lambda identity:native.call("ai.operation",{"operationId":identity})))
        if path not in {"invoke","plan","execute"} or not changed:
            raise ToolError("not_found")
        key=accounts.bind(actor)
        with accounts.use(key) as services:
            if services is None:
                raise ToolError("unavailable")
            store=NebulaWorkspace(services.storage,native,ledger,fleet)
            tools=ToolRuntime(REGISTRY,SqliteAuditSink(ledger.database));bind_nebula(tools,store)
            runtime=SkillRuntime(tools,store,bridge)
            if path == "plan":
                packet(obj({"intent":text(96),"parameters":{"type":"object"}}),body)
                return reply(handler,runtime.planner.plan(body["intent"],body["parameters"]))
            if path == "invoke":
                packet(obj({"skillId":text(96),"version":integer(1,1000),"parameters":{"type":"object"}}),body)
                if REGISTRY.skill(body["skillId"],body["version"]).effect == Effect.EXTERNAL:
                    raise ToolError("denied")
                return reply(handler,runtime.invoke(body["skillId"],body["parameters"],version=body["version"],actor_id=actor))
            packet(obj({"requestId":ID}),body)
            def invoke(operation,parameters):
                validate_operation(operation,parameters,native)
                if operation.startswith("ai."):
                    return {"ok":True,"submitted":True,"value":native.call(operation,parameters),"source":"nebula.model.admin"}
                if operation == "study.undo_write":
                    return store.undo_write(parameters,time.monotonic()+3)
                confirmation=runtime.confirm(actor,operation,parameters)
                return runtime.invoke(operation,parameters,actor_id=actor,confirmation=confirmation)
            return reply(handler,ledger.execute(actor,body["requestId"],invoke))
    except (ToolError,SchemaError,ValueError,KeyError,TypeError) as error:
        code=error.code if isinstance(error,ToolError) else "not_found" if isinstance(error,KeyError) else "invalid"
        handler.reply({"denied":403,"not_found":404,"conflict":409,"unavailable":503,"timeout":504}.get(code,400),{"ok":False,"error":{"code":code}}, {"Cache-Control":"no-store"})
        return True


def reply(handler,value):
    if len(encode(value)) > 131072:
        raise ToolError("invalid")
    handler.reply(200,value,{"Cache-Control":"no-store"})
    return True
