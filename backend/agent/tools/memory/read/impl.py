"""Owner-scoped implementations for memory.read; authorization stays in the adapter."""
from datetime import datetime, timezone
import json
import time

from backend.agent.tool_runtime.runtime import ToolError
from backend.agent.tool_runtime.sources import observed


def read_workspace(store, p, ctx):
    from backend.agent.tool_runtime.workspace import connect
    actor = ctx.actor_id
    with connect(store.database) as db:
        value = [dict(r) for r in db.execute('SELECT id,text FROM memories WHERE actor=? ORDER BY id LIMIT ?', (actor, p['limit']))]
    return observed(value, store.SOURCE)


def read_nebula(store, p, ctx):
    with store.locked(ctx.deadline) as db:
        rows = store.records(db, 'notes', "json_extract(j.value,'$.source')='agent-memory'", (), p['limit'])
        value = [{'id': r['id'], 'text': str(r.get('content', ''))[:2048]} for r in rows]
    return observed(value, store.SOURCE)
