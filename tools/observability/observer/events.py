"""Versioned event contracts. Free text and unregistered attributes are rejected."""
import datetime as dt
import math
import re

IDENT = re.compile(r"[A-Za-z0-9_.-]{1,80}")
DECIMAL = re.compile(r"(0|[1-9][0-9]{0,19})")
HEX = {n: re.compile(r"[0-9a-f]{%d}" % n) for n in (16, 32)}
MAX_UINT64 = (1 << 64) - 1

SCHEMAS = {
    "dag.run": ("runId state", "workflow"),
    "dag.edge": ("runId predecessor successor", ""),
    "dag.node": ("runId nodeId attempt state", "durationMs inputCount outputCount errorCode workerId"),
    "agent.plan": ("agentRunId state", "durationMs stepCount inputTokens outputTokens"),
    "agent.skill": ("agentRunId callId skill attempt state", "durationMs inputCount outputCount inputTokens outputTokens errorCode"),
    "agent.memory": ("agentRunId operation state", "durationMs itemCount"),
    "agent.decision": ("agentRunId decision state", "reasonCode"),
    "agent.evaluation": ("agentRunId evaluator state", "reasonCode"),
    "device.window": ("deviceId sessionId stream samples gaps duplicates late clockQuality", "firstSeq lastSeq capturedAt offsetMs dropped minValue maxValue"),
    "device.command": ("deviceId sessionId commandId state", "action durationMs errorCode"),
    "device.safety": ("deviceId sessionId state reasonCode", "commandId"),
    "device.recording": ("deviceId sessionId recordingId", "stream"),
    "experiment.marker": ("runId state", "scenario"),
    "service.event": ("state operation", "durationMs errorCode"),
}

WORDS = {
    "dag.run": {"state": "started success failed timeout cancelled uncertain"},
    "dag.node": {"state": "queued started success failed timeout retry skipped cancelled uncertain"},
    "agent.plan": {"state": "success failed timeout cancelled"},
    "agent.skill": {"state": "started retry success failed timeout invalid cancelled"},
    "agent.memory": {"operation": "read write", "state": "success failed blocked"},
    "agent.decision": {"decision": "permission refusal validation", "state": "blocked allowed failed"},
    "agent.evaluation": {"state": "flagged passed unknown"},
    "device.window": {"clockQuality": "synchronized unsynchronized unknown"},
    "device.command": {"state": "sent ack error timeout"},
    "device.safety": {"state": "estop cleared"},
    "experiment.marker": {"state": "started fault recovered finished version"},
    "service.event": {"state": "success failed timeout rejected"},
}
INTEGERS = set("attempt inputCount outputCount stepCount itemCount inputTokens outputTokens samples gaps duplicates late dropped".split())
NUMBERS = set("durationMs offsetMs minValue maxValue".split())


class Invalid(ValueError):
    pass


def identifier(value):
    return isinstance(value, str) and bool(IDENT.fullmatch(value))


def unsigned(value):
    return isinstance(value, str) and bool(DECIMAL.fullmatch(value)) and int(value) <= MAX_UINT64


def hex_id(value, width):
    return isinstance(value, str) and bool(HEX[width].fullmatch(value)) and any(c != "0" for c in value)


def validate(value, now_ns, retention_ns, domain):
    required = {"schemaVersion", "eventId", "eventType", "occurredAt", "fields"}
    if not isinstance(value, dict) or not required <= value.keys() <= required | {"traceId", "spanId"}:
        raise Invalid("事件字段不符合契约")
    kind, occurred, fields = (value[k] for k in ("eventType", "occurredAt", "fields"))
    if type(value["schemaVersion"]) is not int or value["schemaVersion"] != 1 or not identifier(value["eventId"]):
        raise Invalid("事件版本或编号无效")
    if not isinstance(kind, str) or kind not in SCHEMAS or kind.split(".")[0] != domain:
        raise Invalid("来源未授权此事件类型")
    if not unsigned(occurred) or not now_ns - retention_ns <= int(occurred) <= now_ns + 300_000_000_000:
        raise Invalid("事件时间超出接收窗口")
    if "traceId" in value and not hex_id(value["traceId"], 32):
        raise Invalid("traceId 无效")
    if "spanId" in value and ("traceId" not in value or not hex_id(value["spanId"], 16)):
        raise Invalid("spanId 无效")
    if domain in ("dag", "agent") and "traceId" not in value:
        raise Invalid("该事件需要 traceId")
    mandatory, optional = (set(x.split()) for x in SCHEMAS[kind])
    if not isinstance(fields, dict) or not mandatory <= fields.keys() <= mandatory | optional:
        raise Invalid("事件属性未登记")
    for key, item in fields.items():
        if key in WORDS.get(kind, {}):
            ok = isinstance(item, str) and item in WORDS[kind][key].split()
        elif key in INTEGERS:
            ok = type(item) is int and (1 if key == "attempt" else 0) <= item <= 1_000_000_000
        elif key in NUMBERS:
            ok = type(item) in (int, float) and abs(item) < 1e12 and math.isfinite(item)
            ok = ok and (key != "durationMs" or item >= 0)
        elif key.endswith("Seq") or key == "capturedAt":
            ok = unsigned(item)
        else:
            ok = identifier(item)
        if not ok:
            raise Invalid("事件属性值无效")
    if kind == "dag.node" and "spanId" not in value:
        raise Invalid("节点执行尝试需要 spanId")
    if kind == "dag.edge" and fields["predecessor"] == fields["successor"]:
        raise Invalid("节点不能依赖自身")
    if "firstSeq" in fields and "lastSeq" in fields and int(fields["firstSeq"]) > int(fields["lastSeq"]):
        raise Invalid("帧区间无效")
    return value


def utc_ns(value):
    if not isinstance(value, str) or not value.endswith("Z"):
        raise Invalid("时间必须是 UTC ISO 8601")
    try:
        actual = dt.datetime.fromisoformat(value[:-1] + "+00:00")
        return int(actual.timestamp() * 1_000_000_000)
    except (ValueError, OverflowError) as error:
        raise Invalid("无效的 UTC 时间") from error
