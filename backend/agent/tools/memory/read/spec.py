"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import ToolSpec, Effect, Precondition

SPEC = ToolSpec(
    name='memory.read',
    parameters={'type': 'object',
     'properties': {'limit': {'type': 'integer', 'minimum': 1, 'maximum': 50}},
     'required': ['limit'],
     'additionalProperties': False},
    max_bytes=8192,
    timeout_ms=1000,
    max_age_seconds=30,
    effect=Effect.READ,
    idempotency_key=None,
    max_items=50,
    version=1,
)
