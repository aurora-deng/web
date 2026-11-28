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
    """Load checked-in contracts in their stable v1 order, never external plugins."""
    from .catalog import load_specs

    registry = Registry()
    for spec in load_specs("tools"):
        registry.add_tool(spec)
    for spec in load_specs("skills"):
        registry.add_skill(spec)
    return registry
