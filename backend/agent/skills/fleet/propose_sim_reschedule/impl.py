"""Deterministic recipe; authorization and audit stay in SkillRuntime."""
from backend.agent.tool_runtime.runtime import ToolError


def run(runtime, p, call, actor, deadline):
    task = call('fleet.get_task_status', p)
    if task['phase'] != 'queued' or task['robotId'] or task['needsReconciliation']:
        raise ToolError('denied')
    if runtime.fleet_bridge is None:
        raise ToolError('unavailable')
    review = runtime.fleet_bridge.call('stage', p['taskId'], deadline)
    return runtime._stage('fleet.propose_sim_reschedule', p, {'kind': 'simulation_review', 'review': review}, actor)
