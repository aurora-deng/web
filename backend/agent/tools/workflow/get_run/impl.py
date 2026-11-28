"""Owner-scoped implementations for workflow.get_run; authorization stays in the adapter."""
from datetime import datetime, timezone
import json
import time

from backend.agent.tool_runtime.runtime import ToolError
from backend.agent.tool_runtime.sources import observed


def read_workspace(store, p, ctx):
    from backend.agent.tool_runtime.workspace import NativeFleetPort
    if store.fleet is None:
        raise ToolError('unavailable')
    value = store.fleet.trace(p)
    source = 'workflow.checkpoint.projection' if 'workflow.get_run' == 'workflow.get_run' and isinstance(store.fleet, NativeFleetPort) else 'workflow.sqlite.checkpoint'
    stamp = observed(value, source)
    if 'workflow.get_run' == 'workflow.get_run' and isinstance(store.fleet, NativeFleetPort):
        from backend.agent.tool_runtime.sources import SourceValue
        stamp = SourceValue(value, source, datetime.fromtimestamp(store.fleet.checkpoint.stat().st_mtime, timezone.utc), True)
    return stamp


def read_nebula(store, p, ctx):
    if store.fleet is None:
        raise ToolError('unavailable')
    return observed(store.fleet.trace(p), 'workflow.checkpoint.projection', fresh=False)
