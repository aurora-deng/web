"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import ToolSpec, Effect, Precondition

SPEC = ToolSpec(
    name='quiz.next',
    parameters={'type': 'object',
     'properties': {'subject': {'type': 'string',
                                'minLength': 1,
                                'maxLength': 64,
                                'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'},
                    'excludeMastered': {'type': 'boolean', 'const': True}},
     'required': ['subject', 'excludeMastered'],
     'additionalProperties': False},
    max_bytes=8192,
    timeout_ms=1000,
    max_age_seconds=30,
    effect=Effect.READ,
    idempotency_key=None,
    max_items=None,
    version=1,
)
