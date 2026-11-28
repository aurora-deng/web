"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import SkillSpec, Effect, Precondition

SPEC = SkillSpec(
    name='fleet.fleet.overview',
    effect=Effect.READ,
    parameters={'type': 'object',
     'properties': {'entity': {'type': 'string', 'minLength': 1, 'maxLength': 5, 'enum': ['tasks']},
                    'limit': {'type': 'integer', 'minimum': 1, 'maximum': 16}},
     'required': ['entity', 'limit'],
     'additionalProperties': False},
    preconditions=(Precondition.AUTHORIZED_ACTOR,),
    timeout_ms=1000,
    idempotency_key=None,
    version=1,
    description='List at most 16 task records, not robots or global fleet statistics.',
)
