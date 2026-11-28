"""Owner-scoped implementations for quiz.next; authorization stays in the adapter."""
from datetime import datetime, timezone
import json
import time

from backend.agent.tool_runtime.runtime import ToolError
from backend.agent.tool_runtime.sources import observed


def read_workspace(store, p, ctx):
    from backend.agent.tool_runtime.workspace import connect
    actor = ctx.actor_id
    with connect(store.database) as db:
        row = db.execute('SELECT id,question,answer FROM questions WHERE actor=? AND subject=? AND mastered=0 ORDER BY id LIMIT 1', (actor, p['subject'])).fetchone()
        if not row:
            raise ToolError('not_found')
        value = dict(row)
    return observed(value, store.SOURCE)


def read_nebula(store, p, ctx):
    with store.locked(ctx.deadline) as db:
        rows = store.records(db, 'flashcards', "coalesce(json_extract(j.value,'$.subjectId'),json_extract(j.value,'$.subject'),'')=? AND coalesce(json_extract(j.value,'$.mastery'),'new') NOT IN ('mastered','done')", (p['subject'],), 1)
        if not rows:
            raise ToolError('not_found')
        row = rows[0]
        value = {'id': row['id'], 'question': str(row.get('front', ''))[:2048], 'answer': str(row.get('back', ''))[:2048]}
    return observed(value, store.SOURCE)
