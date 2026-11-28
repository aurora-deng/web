"""Deterministic recipe; authorization and audit stay in SkillRuntime."""


def run(runtime, p, call, actor, deadline):
    call('preference.get', {})
    return runtime._stage('chat.escalate', p, {'kind': 'human_handoff_request', 'reason': p['reason'], 'contacted': False}, actor)
