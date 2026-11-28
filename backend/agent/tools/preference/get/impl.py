"""Owner-scoped implementations for preference.get; authorization stays in the adapter."""
from datetime import datetime, timezone
import json
import time

from backend.agent.tool_runtime.runtime import ToolError
from backend.agent.tool_runtime.sources import observed


def read_workspace(store, p, ctx):
    from backend.agent.tool_runtime.workspace import connect
    actor = ctx.actor_id
    with connect(store.database) as db:
        row = db.execute('SELECT value FROM preferences WHERE actor=?', (actor,)).fetchone()
        value = json.loads(row[0]) if row else {}
    return observed(value, store.SOURCE)


def read_nebula(store, p, ctx):
    with store.locked(ctx.deadline) as db:
        row = db.execute("SELECT json_extract(body,'$.planCapacities') FROM document WHERE id=1").fetchone()
        capacities = json.loads(row[0]) if row and row[0] else {}
        daily = capacities.get('dailyMinutes', [])
        value = {'dailyMinutes': min(1440, max(0, sum(daily) // len(daily))) if daily and all((type(n) is int for n in daily)) else 60}
    return observed(value, store.SOURCE)
