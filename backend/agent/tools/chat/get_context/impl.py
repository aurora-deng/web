"""Owner-scoped implementations for chat.get_context; authorization stays in the adapter."""
from datetime import datetime, timezone
import json
import time

from backend.agent.tool_runtime.runtime import ToolError
from backend.agent.tool_runtime.sources import observed


def read_workspace(store, p, ctx):
    from backend.agent.tool_runtime.workspace import connect
    actor = ctx.actor_id
    with connect(store.database) as db:
        value = store._context(db, p, actor)
    return observed(value, store.SOURCE)


def read_nebula(store, p, ctx):
    params = p
    response = store.native.call('chat.context', params, ctx.deadline)
    rows = response['items'] if params['radius'] else [response['target']]
    value = [store.message(r) for r in rows]
    return observed(value, 'nebula.chat.authorized')
