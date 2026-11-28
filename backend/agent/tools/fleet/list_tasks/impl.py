"""Owner-scoped implementations for fleet.list_tasks; authorization stays in the adapter."""
from datetime import datetime, timezone
import json
import time

from backend.agent.tool_runtime.runtime import ToolError
from backend.agent.tool_runtime.sources import observed


def read_workspace(store, p, ctx):
    from backend.agent.tool_runtime.workspace import NativeFleetPort
    if store.fleet is None:
        raise ToolError('unavailable')
    value = store.fleet.tasks(p, ctx.deadline) if isinstance(store.fleet, NativeFleetPort) else store.fleet.tasks(p)
    source = 'fleet.grpc.live' if isinstance(store.fleet, NativeFleetPort) else 'fleet.sqlite.snapshot'
    stamp = observed(value, source)
    return stamp


def read_nebula(store, p, ctx):
    if store.fleet is None:
        raise ToolError('unavailable')
    value = store.fleet.tasks(p, ctx.deadline)
    return observed(value, 'fleet.grpc.live')
