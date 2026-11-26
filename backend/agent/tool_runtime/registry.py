"""Versioned, declarative Tool/Skill admission. No prompts or live clients."""
from __future__ import annotations

import copy
import re
from dataclasses import dataclass
from enum import Enum
from types import MappingProxyType

from .schema import SchemaError, check_schema


class Precondition(str, Enum):
    AUTHORIZED_ACTOR = "authorized_actor"
    HUMAN_APPROVAL = "human_approval"
    CONFIRMED_USER_INTENT = "confirmed_user_intent"
    ATOMIC_IDEMPOTENT_PORT = "atomic_idempotent_port"


class Effect(str, Enum):
    READ = "read"
    PROPOSAL = "proposal"
    EXTERNAL = "external"


@dataclass(frozen=True)
class ToolSpec:
    name: str
    parameters: dict
    max_bytes: int
    timeout_ms: int = 1000
    max_age_seconds: int = 30
    effect: Effect = Effect.READ
    idempotency_key: str | None = None
    max_items: int | None = None
    version: int = 1


@dataclass(frozen=True)
class SkillSpec:
    name: str
    effect: Effect
    parameters: dict
    preconditions: tuple[Precondition, ...]
    timeout_ms: int
    idempotency_key: str | None = None
    version: int = 1
    description: str = ""


_INTERNAL = {"database", "idatabase", "itransaction", "postgresstore", "transaction", "sql", "shell", "filesystem", "fetch", "begin", "commit", "rollback"}


def _name(name):
    if (not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+", name)
            or any(part in _INTERNAL for part in name.split("."))):
        raise ValueError("internal_or_invalid_name")


def _parameters(spec):
    _name(spec.name)
    if type(spec.version) is not int or spec.version < 1 or not isinstance(spec.effect, Effect):
        raise ValueError("invalid_version_or_effect")
    if type(spec.timeout_ms) is not int or not 1 <= spec.timeout_ms <= 30000:
        raise ValueError("invalid_timeout")
    check_schema(spec.parameters)
    if spec.parameters["type"] != "object":
        raise ValueError("parameters_must_be_object")
    if spec.effect == Effect.EXTERNAL:
        key = spec.idempotency_key
        field = spec.parameters["properties"].get(key)
        if not key or key not in spec.parameters["required"] or not field or field["type"] != "string" or field.get("minLength", 0) < 1:
            raise ValueError("write_requires_idempotency_key")
    elif spec.idempotency_key is not None:
        raise ValueError("unexpected_idempotency_key")


class Registry:
    def __init__(self):
        self._tools = {}
        self._skills = {}

    def add_tool(self, spec: ToolSpec):
        _parameters(spec)
        if spec.effect == Effect.PROPOSAL:
            raise ValueError("proposals_are_skills_not_write_ports")
        if type(spec.max_bytes) is not int or not 512 <= spec.max_bytes <= 65536:
            raise ValueError("result_must_be_bounded")
        if type(spec.max_age_seconds) is not int or not 0 <= spec.max_age_seconds <= 86400:
            raise ValueError("invalid_freshness_budget")
        if spec.max_items is not None and (type(spec.max_items) is not int or not 1 <= spec.max_items <= 256):
            raise ValueError("invalid_item_bound")
        if (spec.name, spec.version) in self._tools:
            raise ValueError("duplicate_tool")
        self._tools[spec.name, spec.version] = copy.deepcopy(spec)

    def add_skill(self, spec: SkillSpec):
        _parameters(spec)
        if (not spec.preconditions or any(not isinstance(p, Precondition) for p in spec.preconditions) or len(set(spec.preconditions)) != len(spec.preconditions)
                or (spec.effect == Effect.PROPOSAL and "human_approval" not in spec.preconditions)):
            raise ValueError("missing_precondition")
        if Precondition.AUTHORIZED_ACTOR not in spec.preconditions:
            raise ValueError("missing_authorization")
        if spec.effect == Effect.EXTERNAL and not {Precondition.CONFIRMED_USER_INTENT, Precondition.ATOMIC_IDEMPOTENT_PORT} <= set(spec.preconditions):
            raise ValueError("missing_write_precondition")
        if (spec.name, spec.version) in self._skills:
            raise ValueError("duplicate_skill")
        self._skills[spec.name, spec.version] = copy.deepcopy(spec)

    @property
    def tools(self):
        return MappingProxyType({name: copy.deepcopy(spec) for (name, version), spec in self._tools.items() if version == 1})

    @property
    def skills(self):
        return MappingProxyType({name: copy.deepcopy(spec) for (name, version), spec in self._skills.items() if version == 1})

    def tool(self, name, version=1):
        return copy.deepcopy(self._tools[name, version])

    def skill(self, name, version=1):
        return copy.deepcopy(self._skills[name, version])


def text(maximum=128, *, pattern=None, enum=None):
    value = {"type": "string", "minLength": 1, "maxLength": maximum}
    if pattern is not None:
        value["pattern"] = pattern
    if enum is not None:
        value["enum"] = enum
    return value


def integer(low, high):
    return {"type": "integer", "minimum": low, "maximum": high}


def obj(properties, required=None):
    return {"type": "object", "properties": properties,
            "required": list(properties) if required is None else required, "additionalProperties": False}


def array(items, maximum, minimum=0):
    return {"type": "array", "items": items, "minItems": minimum, "maxItems": maximum, "uniqueItems": True}


ID = text(64, pattern=r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}")
KEY = text(128, pattern=r"[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}")
METRICS = ["fleet.active_tasks", "fleet.accept_latency_ms", "chat.ack_latency_ms", "workflow.failed_runs"]
ANCHOR = obj({"kind": text(4, enum=["page", "time"]), "offset": integer(0, 86400000)})


def default_registry():
    registry = Registry()
    def tool(name, fields, *, size=8192, items=None, write=False, required=None, timeout=1000):
        registry.add_tool(ToolSpec(name, obj(fields, required), size, timeout_ms=timeout, max_items=items,
                                  effect=Effect.EXTERNAL if write else Effect.READ,
                                  idempotency_key="idempotencyKey" if write else None))
    tool("fleet.get_task_status", {"taskId": ID}, timeout=1500)
    tool("fleet.list_tasks", {"limit": integer(1, 16), "cursor": ID}, required=["limit"], items=16)
    tool("evidence.get", {"evidenceId": ID}, size=4096)
    tool("workflow.get_run", {"runId": ID, "nodeLimit": integer(1, 64)}, items=64)
    tool("metrics.get", {"names": array(text(40, enum=METRICS), 4, 1), "windowSeconds": integer(1, 86400)}, items=256, timeout=3000)
    tool("chat.search", {"query": text(256), "scope": text(12, enum=["conversation", "workspace"]), "conversationId": ID,
                         "limit": integer(1, 50)}, required=["query", "scope", "limit"], items=50)
    tool("chat.get_context", {"conversationId": ID, "messageId": ID, "radius": integer(0, 20)}, items=41)
    tool("chat.tree_node", {"conversationId": ID, "nodeId": ID})
    tool("memory.read", {"limit": integer(1, 50)}, items=50)
    tool("memory.write", {"text": text(2048), "idempotencyKey": KEY}, write=True)
    tool("preference.get", {})
    material = {"query": text(256), "subject": ID, "type": text(8, enum=["note", "pdf", "video", "document"]), "limit": integer(1, 20)}
    tool("material.search", material, items=20, timeout=2000)
    tool("material.chunk", {"materialId": ID, "anchor": ANCHOR}, size=4096)
    tool("material.stats", {})
    tool("quiz.next", {"subject": ID, "excludeMastered": {"type": "boolean", "const": True}})
    tool("progress.read", {"subject": ID, "limit": integer(1, 50)}, items=50)
    progress = {"date": text(10, pattern=r"\d{4}-\d{2}-\d{2}"), "subject": ID,
                "itemId": ID, "minutes": integer(0, 1440), "idempotencyKey": KEY}
    tool("progress.write", progress, write=True, timeout=2000)
    tool("mistake.write", {"subject": ID, "itemId": ID, "text": text(2048), "idempotencyKey": KEY}, write=True, timeout=2000)

    def skill(name, effect, fields, description, *, timeout=1000, pre=None, idem=None):
        conditions = [Precondition.AUTHORIZED_ACTOR]
        if effect == Effect.PROPOSAL: conditions.append(Precondition.HUMAN_APPROVAL)
        if effect == Effect.EXTERNAL: conditions += [Precondition.CONFIRMED_USER_INTENT, Precondition.ATOMIC_IDEMPOTENT_PORT]
        registry.add_skill(SkillSpec(name, effect, obj(fields), tuple(conditions), timeout, idem, description=description))
    # Sorted by domain and purpose, not historical append order. No duplicate status skill.
    skill("chat.draft_reply", Effect.PROPOSAL, {"conversationId": ID, "messageId": ID, "radius": integer(0,20)},
          "Draft a reply from a bounded symmetric context; never sends a message.", timeout=2000)
    skill("chat.escalate", Effect.PROPOSAL, {"conversationId": ID, "reason": text(256)},
          "Stage a human handoff; never contacts an external system.")
    skill("chat.recall", Effect.READ, {k:v for k,v in registry.tool("chat.search").parameters["properties"].items() if k != "conversationId"} | {"conversationId": ID},
          "Search only this conversation, or the authenticated actor workspace; always bounded.", timeout=1500)
    skill("chat.remind", Effect.PROPOSAL, {"conversationId": ID, "messageId": ID},
          "Stage a reminder for a specific authorized message; never delivers notifications.")
    skill("chat.summarize", Effect.READ, {"conversationId": ID, "messageId": ID, "radius": integer(0,20)},
          "Extractive summary of at most 41 messages around one anchor, not whole-history summarization.", timeout=2000)
    skill("fleet.fleet.overview", Effect.READ, {"entity": text(5, enum=["tasks"]), "limit": integer(1,16)},
          "List at most 16 task records, not robots or global fleet statistics.")
    skill("fleet.get_task_status", Effect.READ, {"taskId": ID}, "Read one task status without mutation.", timeout=1500)
    skill("fleet.propose_sim_reschedule", Effect.PROPOSAL, {"taskId": ID},
          "Stage a replacement of one queued simulation task; approval uses the existing C++ review path.", timeout=2500)
    skill("fleet.review_failures", Effect.PROPOSAL, {"taskIds": array(ID,3,1)},
          "Stage a diagnostic review of up to three failed or uncertain tasks, never reassigns them.", timeout=2500)
    skill("fleet.robot.quarantine", Effect.PROPOSAL, {"robotId": ID, "reason": text(256)},
          "Record an operator quarantine request only; no robot control port is exposed.")
    skill("fleet.task.reassign", Effect.PROPOSAL, {"taskId": ID, "robotId": ID},
          "Record a targeted human reassignment request; not a batch failure review.")
    skill("metrics.query", Effect.READ, registry.tool("metrics.get").parameters["properties"],
          "Read allowlisted, actually sampled metrics over a bounded window; absent metrics fail unavailable.", timeout=4000)
    skill("study.explain", Effect.READ, {"materialId": ID, "anchor": ANCHOR},
          "Return one located material excerpt, at most 4KB; never reads an entire attachment.", timeout=1500)
    skill("study.log_progress", Effect.EXTERNAL, progress,
          "Commit one confirmed progress record and its replay receipt atomically.", timeout=2500, idem="idempotencyKey")
    skill("study.mistake_add", Effect.EXTERNAL, registry.tool("mistake.write").parameters["properties"],
          "Commit one confirmed mistake note and its replay receipt atomically.", timeout=2500, idem="idempotencyKey")
    skill("study.plan_day", Effect.PROPOSAL, {"date": progress["date"], "subject": ID},
          "Stage a bounded daily plan from stored progress and preferences; no calendar write.", timeout=2000)
    skill("study.quiz", Effect.READ, {"subject": ID}, "Select one stored unmastered question, not model-generated scoring.", timeout=1500)
    skill("study.search_material", Effect.READ, material,
          "Search indexed metadata for one subject and material type, limit at most 20; never scans attachment bytes.", timeout=2500)
    skill("workflow.run.trace", Effect.READ, {"runId": ID, "nodeLimit": integer(1,64)},
          "Read persisted checkpoints of one run, at most 64 nodes.")
    # Conditional scope is also checked semantically by both runtime entry points.
    recall = registry._skills["chat.recall", 1]
    registry._skills["chat.recall", 1] = SkillSpec(recall.name, recall.effect,
        obj(recall.parameters["properties"], ["query", "scope", "limit"]), recall.preconditions,
        recall.timeout_ms, description=recall.description)
    return registry
