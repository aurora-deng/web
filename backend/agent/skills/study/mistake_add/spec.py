"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import SkillSpec, Effect, Precondition

SPEC = SkillSpec(
    name='study.mistake_add',
    effect=Effect.EXTERNAL,
    parameters={'type': 'object',
     'properties': {'subject': {'type': 'string',
                                'minLength': 1,
                                'maxLength': 64,
                                'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'},
                    'itemId': {'type': 'string',
                               'minLength': 1,
                               'maxLength': 64,
                               'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'},
                    'text': {'type': 'string', 'minLength': 1, 'maxLength': 2048},
                    'idempotencyKey': {'type': 'string',
                                       'minLength': 1,
                                       'maxLength': 128,
                                       'pattern': '[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}'}},
     'required': ['subject', 'itemId', 'text', 'idempotencyKey'],
     'additionalProperties': False},
    preconditions=(Precondition.AUTHORIZED_ACTOR, Precondition.CONFIRMED_USER_INTENT, Precondition.ATOMIC_IDEMPOTENT_PORT,),
    timeout_ms=2500,
    idempotency_key='idempotencyKey',
    version=1,
    description='Commit one confirmed mistake note and its replay receipt atomically.',
)
