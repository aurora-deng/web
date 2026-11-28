"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import SkillSpec, Effect, Precondition

SPEC = SkillSpec(
    name='chat.summarize',
    effect=Effect.READ,
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
    preconditions=(Precondition.AUTHORIZED_ACTOR,),
    timeout_ms=2000,
    idempotency_key=None,
    version=1,
    description='Extractive summary of at most 41 messages around one anchor, not whole-history summarization.',
)
