"""Owner-scoped implementations for chat.tree_node; authorization stays in the adapter."""
from datetime import datetime, timezone
import json
import time

from backend.agent.tool_runtime.runtime import ToolError
from backend.agent.tool_runtime.sources import observed


def read_workspace(store, p, ctx):
    from backend.agent.tool_runtime.workspace import connect
    actor = ctx.actor_id
    with connect(store.database) as db:
        row = db.execute('SELECT id,parent,substr(text,1,2048) AS text FROM messages WHERE actor=? AND conversation=? AND id=?', (actor, p['conversationId'], p['nodeId'])).fetchone()
        if not row:
            raise ToolError('not_found')
        value = dict(row)
    return observed(value, store.SOURCE)


def read_nebula(store, p, ctx):
    params = {'conversationId': p['conversationId'], 'messageId': p['nodeId'], 'radius': 0}
    response = store.native.call('chat.context', params, ctx.deadline)
    rows = response['items'] if params['radius'] else [response['target']]
    value = [store.message(r) for r in rows]
    value = value[0]
    return observed(value, 'nebula.chat.authorized')
