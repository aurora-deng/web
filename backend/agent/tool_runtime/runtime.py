"""Fail-closed calls, byte-bounded replies, redacted audit, cooperative deadlines."""
from __future__ import annotations

import copy
import hashlib
import json
import math
import re
import sqlite3
import threading
import time
from dataclasses import asdict, dataclass
from contextlib import closing
from datetime import date, datetime, timezone

from .registry import Effect, Registry
from .schema import SchemaError, validate
from .sources import Binding, CallContext, SourceValue


class ToolError(Exception):
    CODES = {"not_found", "denied", "timeout", "invalid", "conflict", "unavailable"}

    def __init__(self, code):
        if code not in self.CODES:
            raise ValueError("unknown_error_code")
        super().__init__(code)
        self.code = code


def encode(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def json_value(value, depth=0, budget=None):
    if budget is None:
        budget = [10000]
    budget[0] -= 1
    if depth > 12 or budget[0] < 0:
        raise ToolError("invalid")
    if value is None or type(value) in {bool, int, str}:
        if isinstance(value, str) and len(value) > 262144:
            raise ToolError("invalid")
        return
    if type(value) is float and math.isfinite(value):
        return
    if type(value) is list:
        for item in value:
            json_value(item, depth + 1, budget)
        return
    if type(value) is dict and all(type(k) is str and len(k) <= 128 for k in value):
        for item in value.values():
            json_value(item, depth + 1, budget)
        return
    raise ToolError("invalid")


def _shrink(value):
    """Preserve object keys (especially anchors/provenance); shrink leaves only."""
    if isinstance(value, str) and value:
        return value[:len(value) // 2], True
    if isinstance(value, list) and value:
        return value[:len(value) // 2], True
    if isinstance(value, dict):
        for key in sorted(value, key=lambda k: len(encode(value[k])), reverse=True):
            new, changed = _shrink(value[key])
            if changed:
                value[key] = new
                return value, True
    return value, False


@dataclass(frozen=True)
class AuditRecord:
    tool: str
    trace_id: str
    actor_hash: str
    parameter_summary: dict
    elapsed_ms: float
    result_code: str
    read_at: str


class MemoryAuditSink:
    def __init__(self, capacity=1024):
        if type(capacity) is not int or not 1 <= capacity <= 100000:
            raise ValueError("invalid_audit_capacity")
        self.records = []
        self.capacity = capacity
        self.lock = threading.Lock()

    def append(self, record):
        with self.lock:
            # No silent audit loss; the next call is denied before its handler.
            if len(self.records) >= self.capacity:
                raise ToolError("unavailable")
            self.records.append(record)


class SqliteAuditSink:
    """Local audit only; constructor path is deployment configuration, never a tool arg."""
    def __init__(self, database):
        self.database = database
        with closing(sqlite3.connect(database)) as connection, connection:
            connection.execute("CREATE TABLE IF NOT EXISTS tool_audit (id INTEGER PRIMARY KEY, record TEXT NOT NULL)")

    def append(self, record):
        with closing(sqlite3.connect(self.database, timeout=1)) as connection, connection:
            connection.execute("INSERT INTO tool_audit(record) VALUES (?)", (encode(asdict(record)).decode(),))


def planner_mode(results):
    """Only complete, fresh observations may enter the model-assisted Planner."""
    return "provider" if results and all(r.get("ok") is True and r.get("fresh") is True and not r.get("truncated") for r in results) else "rules"


class ToolRuntime:
    def __init__(self, registry: Registry, audit):
        self.registry = registry
        self.audit = audit
        self._bindings = {}

    def bind(self, name, binding: Binding):
        spec = self.registry.tool(name)
        if (not isinstance(binding, Binding) or not re.fullmatch(r"[A-Za-z0-9_.:-]{1,96}", binding.source)
                or not callable(binding.authorize)):
            raise ValueError("invalid_binding")
        if spec.effect == Effect.READ:
            if not callable(binding.read) or binding.write is not None:
                raise ValueError("read_binding_required")
        elif binding.read is not None or not callable(getattr(binding.write, "execute_once", None)):
            raise ValueError("atomic_write_port_required")
        if name in self._bindings:
            raise ValueError("duplicate_binding")
        self._bindings[name] = binding

    def call(self, name, parameters, *, actor_id, trace_id, deadline=None):
        started = time.monotonic()
        valid_name = isinstance(name, str) and bool(re.fullmatch(r"[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+", name)) and len(name) <= 96
        valid_trace = isinstance(trace_id, str) and bool(re.fullmatch(r"[A-Za-z0-9_-]{1,64}", trace_id))
        safe_name, safe_trace = (name if valid_name else "invalid"), (trace_id if valid_trace else "invalid")
        valid_actor = isinstance(actor_id, str) and bool(re.fullmatch(r"[A-Za-z0-9_-]{1,64}", actor_id))
        actor_hash = hashlib.sha256(actor_id.encode()).hexdigest() if valid_actor else "invalid"
        summary = {}
        def audit(code):
            self.audit.append(AuditRecord(safe_name, safe_trace, actor_hash, summary,
                              round((time.monotonic() - started) * 1000, 3), code,
                              datetime.now(timezone.utc).isoformat()))
        try:
            if not valid_name or not valid_trace or not valid_actor:
                raise ToolError("invalid")
            try:
                spec = self.registry.tool(name)
            except KeyError:
                raise ToolError("not_found") from None
            validate(spec.parameters, parameters)
            if name == "chat.search":
                if (parameters["scope"] == "conversation") != ("conversationId" in parameters):
                    raise ToolError("invalid")
            if "date" in parameters:
                try:
                    date.fromisoformat(parameters["date"])
                except ValueError:
                    raise ToolError("invalid") from None
            if name == "material.chunk" and parameters["anchor"]["kind"] == "page" and parameters["anchor"]["offset"] < 1:
                raise ToolError("invalid")
            # Only schema field names and types, never chat text, IDs, keys or payload hashes.
            summary = {key: type(value).__name__ for key, value in parameters.items()}
            audit("started")  # audit unavailable => no side effect
            binding = self._bindings.get(name)
            if binding is None or binding.authorize(actor_id, name, copy.deepcopy(parameters)) is not True:
                raise ToolError("denied")
            context = CallContext(actor_id, trace_id, min(started + spec.timeout_ms / 1000, deadline) if deadline is not None else started + spec.timeout_ms / 1000)
            if time.monotonic() >= context.deadline:
                raise ToolError("timeout")
            observation = (binding.read(copy.deepcopy(parameters), context) if spec.effect == Effect.READ else
                           binding.write.execute_once(name, copy.deepcopy(parameters), context))
            if time.monotonic() >= context.deadline:
                raise ToolError("timeout")
            if (not isinstance(observation, SourceValue) or observation.source != binding.source
                    or type(observation.fresh) is not bool or not isinstance(observation.read_at, datetime)
                    or observation.read_at.tzinfo is None):
                raise ToolError("invalid")
            age = (datetime.now(timezone.utc) - observation.read_at).total_seconds()
            if age < 0:
                raise ToolError("invalid")
            json_value(observation.value)
            value = copy.deepcopy(observation.value)
            truncated = False
            if spec.max_items is not None:
                # Collection ports must return a list, not hide an unbounded list in a dict.
                if not isinstance(value, list):
                    raise ToolError("invalid")
                bound = min(spec.max_items, parameters.get("limit", parameters.get("nodeLimit", spec.max_items)))
                if name == "chat.get_context":
                    bound = min(bound, 2 * parameters["radius"] + 1)
                if len(value) > bound:
                    value, truncated = value[:bound], True
            if name == "material.search" and any(not isinstance(row, dict) or set(row) - {"materialId", "title", "kind", "anchor", "score"} for row in value):
                raise ToolError("invalid")
            if name == "material.chunk":
                if (not isinstance(value, dict) or set(value) - {"materialId", "anchor", "text"}
                        or value.get("materialId") != parameters["materialId"]
                        or value.get("anchor") != parameters["anchor"] or not isinstance(value.get("text"), str)):
                    raise ToolError("invalid")
                if value["anchor"]["kind"] == "page" and value["anchor"]["offset"] < 1:
                    raise ToolError("invalid")
            result = {"ok": True, "value": value, "source": observation.source,
                      "readAt": observation.read_at.isoformat(), "fresh": observation.fresh and age <= spec.max_age_seconds,
                      "truncated": truncated}
            while len(encode(result)) > spec.max_bytes:
                if name == "material.chunk":
                    clipped, changed = _shrink(result["value"]["text"])
                    result["value"]["text"] = clipped
                else:
                    result["value"], changed = _shrink(result["value"])
                result["truncated"] = True
                if not changed:
                    raise ToolError("invalid")
            audit("ok")
            return result
        except Exception as error:
            if isinstance(error, ToolError):
                code = error.code
            elif isinstance(error, TimeoutError):
                code = "timeout"
            elif isinstance(error, SchemaError):
                code = "invalid"
            else:
                code = "unavailable"
            try:
                audit(code)
            except Exception:
                code = "unavailable"
            # Adapter exception text is never returned (may contain data or credentials).
            return {"ok": False, "error": {"code": code}, "value": None,
                    "source": "runtime", "readAt": datetime.now(timezone.utc).isoformat(),
                    "fresh": False, "truncated": False}
