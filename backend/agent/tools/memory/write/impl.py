"""Business write for memory.write; called inside the adapter-owned receipt transaction."""
from datetime import datetime, timedelta, timezone

def write_workspace(store, db, p, ctx, identity):
    db.execute('INSERT INTO memories VALUES (?,?,?)', (ctx.actor_id, identity, p['text']))

def write_nebula(store, db, p, ctx, identity, now, item):
    col = 'notes'
    record = {'id': identity, 'title': 'Agent memory', 'content': p['text'], 'source': 'agent-memory', 'createdAt': now}
    return col, record
