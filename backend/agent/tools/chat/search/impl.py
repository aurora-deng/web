"""Owner-scoped implementations for chat.search; authorization stays in the adapter."""
from datetime import datetime, timezone
import json
import time

from backend.agent.tool_runtime.runtime import ToolError
from backend.agent.tool_runtime.sources import observed


def read_workspace(store, p, ctx):
    from backend.agent.tool_runtime.workspace import connect
    actor = ctx.actor_id
    with connect(store.database) as db:
        clause = ' AND conversation=?' if p['scope'] == 'conversation' else ''
        values = [actor, p['query']] + ([p['conversationId']] if clause else []) + [p['limit']]
        value = [dict(r) for r in db.execute('SELECT id,conversation,seq,substr(text,1,2048) AS text FROM messages WHERE actor=? AND instr(lower(text),lower(?))>0' + clause + ' ORDER BY conversation,seq LIMIT ?', values)]
    return observed(value, store.SOURCE)


def read_nebula(store, p, ctx):
    value = [store.message(r) for r in store.native.call('chat.search', p, ctx.deadline)['items'][:p['limit']]]
    return observed(value, 'nebula.chat.authorized')
