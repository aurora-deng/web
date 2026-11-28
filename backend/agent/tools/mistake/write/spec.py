"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import ToolSpec, Effect, Precondition

SPEC = ToolSpec(
    name='mistake.write',
    parameters={'type': 'object',
     'properties': {'subject': {'type': 'string',
                                'minLength': 1,
                                'maxLength': 64,
                                'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'},
                    'itemId': {'type': 'string',
                               'minLength': 1,
                               'maxLength': 64,
                               'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'},
                    'text': {'type': 'string', 'minLength': 1, 'maxLength': 2048},
                    'idempotencyKey': {'type': 'string',
                                       'minLength': 1,
                                       'maxLength': 128,
                                       'pattern': '[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}'}},
     'required': ['subject', 'itemId', 'text', 'idempotencyKey'],
     'additionalProperties': False},
    max_bytes=8192,
    timeout_ms=2000,
    max_age_seconds=30,
    effect=Effect.EXTERNAL,
    idempotency_key='idempotencyKey',
    max_items=None,
    version=1,
)
