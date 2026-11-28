"""Owner-scoped implementations for material.search; authorization stays in the adapter."""
from datetime import datetime, timezone
import json
import time

from backend.agent.tool_runtime.runtime import ToolError
from backend.agent.tool_runtime.sources import observed


def read_workspace(store, p, ctx):
    from backend.agent.tool_runtime.workspace import connect
    actor = ctx.actor_id
    with connect(store.database) as db:
        value = [{'materialId': r['id'], 'title': r['title'], 'kind': r['kind']} for r in db.execute('SELECT id,substr(title,1,256) AS title,kind FROM materials WHERE actor=? AND subject=? AND kind=? AND instr(lower(title),lower(?))>0 ORDER BY id LIMIT ?', (actor, p['subject'], p['type'], p['query'], p['limit']))]
    return observed(value, store.SOURCE)


def read_nebula(store, p, ctx):
    with store.locked(ctx.deadline) as db:
        value = []
        for col in ('resources', 'notes'):
            rows = store.records(db, col, "coalesce(json_extract(j.value,'$.subjectId'),json_extract(j.value,'$.subject'),'')=? AND instr(lower(coalesce(json_extract(j.value,'$.title'),'')),lower(?))>0 AND (?='note' AND ?='notes' OR ?!='note' AND ?='resources' AND (coalesce(json_extract(j.value,'$.type'),json_extract(j.value,'$.kind'),'')=? OR lower(coalesce(json_extract(j.value,'$.name'),json_extract(j.value,'$.title'),'')) LIKE ?))", (p['subject'], p['query'], p['type'], col, p['type'], col, p['type'], '%.' + {'document': 'docx'}.get(p['type'], p['type'])), p['limit'] - len(value))
            value.extend(({'materialId': r['id'], 'title': str(r.get('title', r['id']))[:256], 'kind': p['type']} for r in rows))
            if len(value) >= p['limit']:
                break
    return observed(value, store.SOURCE)
