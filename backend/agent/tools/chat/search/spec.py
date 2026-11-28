"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import ToolSpec, Effect, Precondition

SPEC = ToolSpec(
    name='chat.search',
    parameters={'type': 'object',
     'properties': {'query': {'type': 'string', 'minLength': 1, 'maxLength': 256},
                    'scope': {'type': 'string',
                              'minLength': 1,
                              'maxLength': 12,
                              'enum': ['conversation', 'workspace']},
                    'conversationId': {'type': 'string',
                                       'minLength': 1,
                                       'maxLength': 64,
                                       'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'},
                    'limit': {'type': 'integer', 'minimum': 1, 'maximum': 50}},
     'required': ['query', 'scope', 'limit'],
     'additionalProperties': False},
    max_bytes=8192,
    timeout_ms=1000,
    max_age_seconds=30,
    effect=Effect.READ,
    idempotency_key=None,
    max_items=50,
    version=1,
)
