"""Deterministic recipe; authorization and audit stay in SkillRuntime."""


def run(runtime, p, call, actor, deadline):
    return runtime._stage('fleet.robot.quarantine', p, {'kind': 'operator_quarantine_request', 'robotId': p['robotId'], 'reason': p['reason'], 'dispatch': False}, actor)
