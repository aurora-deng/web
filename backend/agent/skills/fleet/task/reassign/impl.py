"""Deterministic recipe; authorization and audit stay in SkillRuntime."""


def run(runtime, p, call, actor, deadline):
    task = call('fleet.get_task_status', {'taskId': p['taskId']})
    return runtime._stage('fleet.task.reassign', p, {'kind': 'operator_reassignment_request', 'task': task, 'targetRobot': p['robotId'], 'dispatch': False}, actor)
