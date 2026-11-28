"""Deterministic recipe; authorization and audit stay in SkillRuntime."""


def run(runtime, p, call, actor, deadline):
    row = call('chat.tree_node', {'conversationId': p['conversationId'], 'nodeId': p['messageId']})
    return runtime._stage('chat.remind', p, {'kind': 'reminder_draft', 'message': row, 'delivered': False}, actor)
