"""Business write for progress.write; called inside the adapter-owned receipt transaction."""
from datetime import datetime, timedelta, timezone

def write_workspace(store, db, p, ctx, identity):
    db.execute('INSERT INTO progress VALUES (?,?,?,?,?,?)', (ctx.actor_id, identity, p['date'], p['subject'], p['itemId'], p['minutes']))

def write_nebula(store, db, p, ctx, identity, now, item):
    col = 'focusSessions'
    start = datetime.fromisoformat(p['date']).replace(tzinfo=timezone.utc)
    record = {'id': identity, 'subject': p['subject'], 'itemId': p['itemId'], 'date': p['date'], 'seconds': p['minutes'] * 60, 'startedAt': start.isoformat(), 'endedAt': (start + timedelta(minutes=p['minutes'])).isoformat(), 'source': 'agent-control', 'activityType': item.get('activityType', 'unclassified'), 'createdAt': now}
    return col, record
