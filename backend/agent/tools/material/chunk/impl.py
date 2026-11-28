"""Owner-scoped implementations for material.chunk; authorization stays in the adapter."""
from datetime import datetime, timezone
import json
import time

from backend.agent.tool_runtime.runtime import ToolError
from backend.agent.tool_runtime.sources import observed


def read_workspace(store, p, ctx):
    from backend.agent.tool_runtime.workspace import connect
    actor = ctx.actor_id
    with connect(store.database) as db:
        a = p['anchor']
        row = db.execute('SELECT text FROM chunks WHERE actor=? AND material=? AND kind=? AND offset=?', (actor, p['materialId'], a['kind'], a['offset'])).fetchone()
        if not row:
            raise ToolError('not_found')
        value = {'materialId': p['materialId'], 'anchor': a, 'text': row[0]}
    return observed(value, store.SOURCE)


def read_nebula(store, p, ctx):
    with store.locked(ctx.deadline) as db:
        col, row = store.record(db, p['materialId'])
        a = p['anchor']
        if col == 'notes' and a == {'kind': 'page', 'offset': 1}:
            text = str(row.get('content', row.get('text', '')))
        elif col == 'resources' and a['kind'] == 'page' and row.get('fileId'):
            found = db.execute('SELECT body FROM attachment_text WHERE id=? AND page=? LIMIT 1', (row['fileId'], a['offset'])).fetchone()
            if not found:
                raise ToolError('not_found')
            cached = json.loads(found[0])
            text = cached.get('text', '')
        else:
            raise ToolError('unavailable')
        if not text:
            raise ToolError('not_found')
        value = {'materialId': p['materialId'], 'anchor': a, 'text': text[:2048]}
    return observed(value, store.SOURCE)
