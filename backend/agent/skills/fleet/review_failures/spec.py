"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import SkillSpec, Effect, Precondition

SPEC = SkillSpec(
    name='fleet.review_failures',
    effect=Effect.PROPOSAL,
    parameters={'type': 'object',
     'properties': {'taskIds': {'type': 'array',
                                'items': {'type': 'string',
                                          'minLength': 1,
                                          'maxLength': 64,
                                          'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'},
                                'minItems': 1,
                                'maxItems': 3,
                                'uniqueItems': True}},
     'required': ['taskIds'],
     'additionalProperties': False},
    preconditions=(Precondition.AUTHORIZED_ACTOR, Precondition.HUMAN_APPROVAL,),
    timeout_ms=2500,
    idempotency_key=None,
    version=1,
    description='Stage a diagnostic review of up to three failed or uncertain tasks, never reassigns them.',
)
