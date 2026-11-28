"""Deterministic recipe; authorization and audit stay in SkillRuntime."""
from backend.agent.tool_runtime.runtime import ToolError


def run(runtime, p, call, actor, deadline):
    tasks = [call('fleet.get_task_status', {'taskId': identity}) for identity in p['taskIds']]
    failed = [task for task in tasks if task['phase'] in {'failed', 'uncertain'} or task['needsReconciliation']]
    if not failed:
        raise ToolError('invalid')
    return runtime._stage('fleet.review_failures', p, {'kind': 'diagnostic_review', 'tasks': failed, 'dispatch': False}, actor)
