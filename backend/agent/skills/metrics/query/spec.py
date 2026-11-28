"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import SkillSpec, Effect, Precondition

SPEC = SkillSpec(
    name='metrics.query',
    effect=Effect.READ,
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
    preconditions=(Precondition.AUTHORIZED_ACTOR,),
    timeout_ms=4000,
    idempotency_key=None,
    version=1,
    description=('Read allowlisted, actually sampled metrics over a bounded window; absent metrics fail '
     'unavailable.'),
)
