"""Deterministic recipe; authorization and audit stay in SkillRuntime."""


def run(runtime, p, call, *_):
    return {'entity': 'tasks', 'tasks': call('fleet.list_tasks', {'limit': p['limit']})}
