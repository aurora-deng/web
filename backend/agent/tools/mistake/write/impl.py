"""Business write for mistake.write; called inside the adapter-owned receipt transaction."""
from datetime import datetime, timedelta, timezone

def write_workspace(store, db, p, ctx, identity):
    db.execute('INSERT INTO mistakes VALUES (?,?,?,?,?)', (ctx.actor_id, identity, p['subject'], p['itemId'], p['text']))

def write_nebula(store, db, p, ctx, identity, now, item):
    col = 'mistakes'
    record = {'id': identity, 'title': p['text'][:120], 'subject': p['subject'], 'itemId': p['itemId'], 'cause': p['text'], 'mastery': 'new', 'source': 'agent-control', 'createdAt': now}
    return col, record
