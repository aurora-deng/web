"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import ToolSpec, Effect, Precondition

SPEC = ToolSpec(
    name='fleet.get_task_status',
    parameters={'type': 'object',
     'properties': {'taskId': {'type': 'string',
                               'minLength': 1,
                               'maxLength': 64,
                               'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'}},
     'required': ['taskId'],
     'additionalProperties': False},
    max_bytes=8192,
    timeout_ms=1500,
    max_age_seconds=30,
    effect=Effect.READ,
    idempotency_key=None,
    max_items=None,
    version=1,
)
