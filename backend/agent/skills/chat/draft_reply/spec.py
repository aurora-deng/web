"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import SkillSpec, Effect, Precondition

SPEC = SkillSpec(
    name='chat.draft_reply',
    effect=Effect.PROPOSAL,
    parameters={'type': 'object',
     'properties': {'conversationId': {'type': 'string',
                                       'minLength': 1,
                                       'maxLength': 64,
                                       'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'},
                    'messageId': {'type': 'string',
                                  'minLength': 1,
                                  'maxLength': 64,
                                  'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'},
                    'radius': {'type': 'integer', 'minimum': 0, 'maximum': 20}},
     'required': ['conversationId', 'messageId', 'radius'],
     'additionalProperties': False},
    preconditions=(Precondition.AUTHORIZED_ACTOR, Precondition.HUMAN_APPROVAL,),
    timeout_ms=2000,
    idempotency_key=None,
    version=1,
    description='Draft a reply from a bounded symmetric context; never sends a message.',
)
