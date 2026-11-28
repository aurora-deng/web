"""Owner-scoped implementations for progress.read; authorization stays in the adapter."""
from datetime import datetime, timezone
import json
import time

from backend.agent.tool_runtime.runtime import ToolError
from backend.agent.tool_runtime.sources import observed


def read_workspace(store, p, ctx):
    from backend.agent.tool_runtime.workspace import connect
    actor = ctx.actor_id
    with connect(store.database) as db:
        value = [dict(r) for r in db.execute('SELECT id,date,item,minutes FROM progress WHERE actor=? AND subject=? ORDER BY date DESC,id LIMIT ?', (actor, p['subject'], p['limit']))]
    return observed(value, store.SOURCE)


def read_nebula(store, p, ctx):
    with store.locked(ctx.deadline) as db:
        rows = store.records(db, 'focusSessions', "coalesce(json_extract(j.value,'$.subjectId'),json_extract(j.value,'$.subject'),'')=?", (p['subject'],), p['limit'])
        value = [{'id': r['id'], 'date': r.get('date', r.get('startedAt', '')[:10]), 'item': r.get('itemId', ''), 'minutes': int(r.get('seconds', 0)) // 60} for r in rows]
    return observed(value, store.SOURCE)
