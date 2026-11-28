"""Owner-scoped implementations for metrics.get; authorization stays in the adapter."""
from datetime import datetime, timezone
import json
import time

from backend.agent.tool_runtime.runtime import ToolError
from backend.agent.tool_runtime.sources import observed


def read_workspace(store, p, ctx):
    from backend.agent.tool_runtime.workspace import connect
    actor = ctx.actor_id
    with connect(store.database) as db:
        now = time.time()
        if store.fleet:
            for metric, number in store.fleet.counts().items():
                db.execute('INSERT INTO metric_samples VALUES (?,?,?)', (metric, now, number))
        db.execute('DELETE FROM metric_samples WHERE at<?', (now - 86400,))
        db.execute('DELETE FROM metric_samples WHERE rowid NOT IN (SELECT rowid FROM metric_samples ORDER BY at DESC LIMIT 10000)')
        value = []
        for metric in p['names']:
            rows = db.execute('SELECT at,value FROM metric_samples WHERE name=? AND at>=? ORDER BY at DESC LIMIT 64', (metric, now - p['windowSeconds'])).fetchall()
            if not rows:
                raise ToolError('unavailable')
            value.extend(({'name': metric, 'sampledAt': datetime.fromtimestamp(r[0], timezone.utc).isoformat(), 'value': r[1]} for r in rows))
    return observed(value, store.SOURCE)


def read_nebula(store, p, ctx):
    if store.fleet is None:
        raise ToolError('unavailable')
    if p['names'] != ['fleet.active_tasks']:
        raise ToolError('unavailable')
    value = [{'name': 'fleet.active_tasks', 'value': store.fleet.counts()['fleet.active_tasks'], 'sampledAt': datetime.now(timezone.utc).isoformat(), 'aggregation': 'instant', 'requestedWindowSeconds': p['windowSeconds']}]
    return observed(value, 'fleet.grpc.live')
