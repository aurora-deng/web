"""Deterministic recipe; authorization and audit stay in SkillRuntime."""


def run(runtime, p, call, actor, deadline):
    progress = call('progress.read', {'subject': p['subject'], 'limit': 50})
    preference = call('preference.get', {})
    used = sum((r['minutes'] for r in progress if r['date'] == p['date']))
    budget = preference.get('dailyMinutes', 60)
    if type(budget) is not int:
        budget = 60
    return runtime._stage('study.plan_day', p, {'kind': 'daily_plan_draft', 'remainingMinutes': max(0, min(1440, budget) - used), 'calendarWritten': False}, actor)
