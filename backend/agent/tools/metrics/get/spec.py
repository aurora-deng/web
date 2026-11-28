"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import ToolSpec, Effect, Precondition

SPEC = ToolSpec(
    name='metrics.get',
    parameters={'type': 'object',
     'properties': {'names': {'type': 'array',
                              'items': {'type': 'string',
                                        'minLength': 1,
                                        'maxLength': 40,
                                        'enum': ['fleet.active_tasks',
                                                 'fleet.accept_latency_ms',
                                                 'chat.ack_latency_ms',
                                                 'workflow.failed_runs']},
                              'minItems': 1,
                              'maxItems': 4,
                              'uniqueItems': True},
                    'windowSeconds': {'type': 'integer', 'minimum': 1, 'maximum': 86400}},
     'required': ['names', 'windowSeconds'],
     'additionalProperties': False},
    max_bytes=8192,
    timeout_ms=3000,
    max_age_seconds=30,
    effect=Effect.READ,
    idempotency_key=None,
    max_items=256,
    version=1,
)
