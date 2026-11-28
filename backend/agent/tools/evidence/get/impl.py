"""Owner-scoped implementations for evidence.get; authorization stays in the adapter."""
from datetime import datetime, timezone
import json
import time

from backend.agent.tool_runtime.runtime import ToolError
from backend.agent.tool_runtime.sources import observed


def read_workspace(store, p, ctx):
    from backend.agent.tool_runtime.workspace import NativeFleetPort
    if store.fleet is None:
        raise ToolError('unavailable')
    value = store.fleet.status(p.get('taskId', p.get('evidenceId')), ctx.deadline) if isinstance(store.fleet, NativeFleetPort) else store.fleet.status(p.get('taskId', p.get('evidenceId')))
    source = 'workflow.checkpoint.projection' if 'evidence.get' == 'workflow.get_run' and isinstance(store.fleet, NativeFleetPort) else 'fleet.grpc.live' if isinstance(store.fleet, NativeFleetPort) else 'fleet.sqlite.snapshot'
    stamp = observed(value, source)
    if 'evidence.get' == 'workflow.get_run' and isinstance(store.fleet, NativeFleetPort):
        from backend.agent.tool_runtime.sources import SourceValue
        stamp = SourceValue(value, source, datetime.fromtimestamp(store.fleet.checkpoint.stat().st_mtime, timezone.utc), True)
    return stamp


def read_nebula(store, p, ctx):
    if store.fleet is None:
        raise ToolError('unavailable')
    value = store.fleet.status(p['evidenceId'], ctx.deadline)
    return observed(value, 'fleet.grpc.live')
