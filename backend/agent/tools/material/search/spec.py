"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import ToolSpec, Effect, Precondition

SPEC = ToolSpec(
    name='material.search',
    parameters={'type': 'object',
     'properties': {'query': {'type': 'string', 'minLength': 1, 'maxLength': 256},
                    'subject': {'type': 'string',
                                'minLength': 1,
                                'maxLength': 64,
                                'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'},
                    'type': {'type': 'string',
                             'minLength': 1,
                             'maxLength': 8,
                             'enum': ['note', 'pdf', 'video', 'document']},
                    'limit': {'type': 'integer', 'minimum': 1, 'maximum': 20}},
     'required': ['query', 'subject', 'type', 'limit'],
     'additionalProperties': False},
    max_bytes=8192,
    timeout_ms=2000,
    max_age_seconds=30,
    effect=Effect.READ,
    idempotency_key=None,
    max_items=20,
    version=1,
)
