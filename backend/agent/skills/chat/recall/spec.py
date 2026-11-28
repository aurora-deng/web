"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import SkillSpec, Effect, Precondition

SPEC = SkillSpec(
    name='chat.recall',
    effect=Effect.READ,
    parameters={'type': 'object',
     'properties': {'query': {'type': 'string', 'minLength': 1, 'maxLength': 256},
                    'scope': {'type': 'string',
                              'minLength': 1,
                              'maxLength': 12,
                              'enum': ['conversation', 'workspace']},
                    'limit': {'type': 'integer', 'minimum': 1, 'maximum': 50},
                    'conversationId': {'type': 'string',
                                       'minLength': 1,
                                       'maxLength': 64,
                                       'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'}},
     'required': ['query', 'scope', 'limit'],
     'additionalProperties': False},
    preconditions=(Precondition.AUTHORIZED_ACTOR,),
    timeout_ms=1500,
    idempotency_key=None,
    version=1,
    description='Search only this conversation, or the authenticated actor workspace; always bounded.',
)
