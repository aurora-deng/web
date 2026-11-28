"""Deterministic recipe; authorization and audit stay in SkillRuntime."""


def run(runtime, p, call, *_):
    rows = call('chat.get_context', p)
    return {'method': 'extractive_rules', 'messageIds': [r['id'] for r in rows], 'summary': ' | '.join((r['text'][:160] for r in rows)), 'radius': p['radius']}
