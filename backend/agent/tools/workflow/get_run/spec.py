"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import ToolSpec, Effect, Precondition

SPEC = ToolSpec(
    name='workflow.get_run',
    parameters={'type': 'object',
     'properties': {'runId': {'type': 'string',
                              'minLength': 1,
                              'maxLength': 64,
                              'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'},
                    'nodeLimit': {'type': 'integer', 'minimum': 1, 'maximum': 64}},
     'required': ['runId', 'nodeLimit'],
     'additionalProperties': False},
    max_bytes=8192,
    timeout_ms=1000,
    max_age_seconds=30,
    effect=Effect.READ,
    idempotency_key=None,
    max_items=64,
    version=1,
)
