"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import SkillSpec, Effect, Precondition

SPEC = SkillSpec(
    name='chat.remind',
    effect=Effect.PROPOSAL,
    parameters={'type': 'object',
     'properties': {'conversationId': {'type': 'string',
                                       'minLength': 1,
                                       'maxLength': 64,
                                       'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'},
                    'messageId': {'type': 'string',
                                  'minLength': 1,
                                  'maxLength': 64,
                                  'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'}},
     'required': ['conversationId', 'messageId'],
     'additionalProperties': False},
    preconditions=(Precondition.AUTHORIZED_ACTOR, Precondition.HUMAN_APPROVAL,),
    timeout_ms=1000,
    idempotency_key=None,
    version=1,
    description='Stage a reminder for a specific authorized message; never delivers notifications.',
)
