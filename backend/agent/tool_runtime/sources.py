"""Trusted, owner-scoped source ports. Neither requests nor model output set provenance."""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Callable, Protocol


@dataclass(frozen=True)
class SourceValue:
    value: object
    source: str
    read_at: datetime
    fresh: bool


@dataclass(frozen=True)
class CallContext:
    actor_id: str
    trace_id: str
    deadline: float  # monotonic seconds; adapters must propagate this deadline


class AtomicWritePort(Protocol):
    """Apply side effect + payload digest + receipt atomically in the source store.

    Equal (actor, tool, key) and equal payload replays the receipt; unequal
    payload raises ToolError('conflict'). Receipt retention must outlive retries.
    An uncertain commit must not be retried under a new key. A runtime-side
    receipt AFTER an arbitrary handler is NOT sufficient for this protocol.
    """
    def execute_once(self, tool: str, parameters: dict, context: CallContext) -> SourceValue: ...


@dataclass(frozen=True)
class Binding:
    source: str
    authorize: Callable[[str, str, dict], bool]
    read: Callable[[dict, CallContext], SourceValue] | None = None
    write: AtomicWritePort | None = None


def observed(value, source: str, *, fresh: bool = True):
    """Only trusted adapters may construct observations."""
    return SourceValue(value, source, datetime.now(timezone.utc), fresh)
