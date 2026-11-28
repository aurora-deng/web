"""Owner-scoped implementations for material.stats; authorization stays in the adapter."""
from datetime import datetime, timezone
import json
import time

from backend.agent.tool_runtime.runtime import ToolError
from backend.agent.tool_runtime.sources import observed


def read_workspace(store, p, ctx):
    from backend.agent.tool_runtime.workspace import connect
    actor = ctx.actor_id
    with connect(store.database) as db:
        value = {'count': db.execute('SELECT COUNT(*) FROM materials WHERE actor=?', (actor,)).fetchone()[0], 'scope': 'actor_index_only'}
    return observed(value, store.SOURCE)


def read_nebula(store, p, ctx):
    with store.locked(ctx.deadline) as db:
        value = {'scope': 'account_metadata_only', 'count': sum((db.execute('SELECT count(*) FROM document d,json_each(d.body,?)', ('$.' + col,)).fetchone()[0] for col in ('resources', 'notes')))}
    return observed(value, store.SOURCE)
