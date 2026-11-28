"""Deterministic recipe; authorization and audit stay in SkillRuntime."""


def run(runtime, p, call, actor, deadline):
    rows = call('chat.get_context', p)
    return runtime._stage('chat.draft_reply', p, {'kind': 'reply_draft', 'text': 'Regarding: ' + rows[-1]['text'][:300], 'messageIds': [r['id'] for r in rows], 'sent': False}, actor)
