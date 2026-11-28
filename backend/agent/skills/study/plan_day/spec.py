"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import SkillSpec, Effect, Precondition

SPEC = SkillSpec(
    name='study.plan_day',
    effect=Effect.PROPOSAL,
    parameters={'type': 'object',
     'properties': {'date': {'type': 'string',
                             'minLength': 1,
                             'maxLength': 10,
                             'pattern': '\\d{4}-\\d{2}-\\d{2}'},
                    'subject': {'type': 'string',
                                'minLength': 1,
                                'maxLength': 64,
                                'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'}},
     'required': ['date', 'subject'],
     'additionalProperties': False},
    preconditions=(Precondition.AUTHORIZED_ACTOR, Precondition.HUMAN_APPROVAL,),
    timeout_ms=2000,
    idempotency_key=None,
    version=1,
    description='Stage a bounded daily plan from stored progress and preferences; no calendar write.',
)
