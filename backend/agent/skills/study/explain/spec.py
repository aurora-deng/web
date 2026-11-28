"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import SkillSpec, Effect, Precondition

SPEC = SkillSpec(
    name='study.explain',
    effect=Effect.READ,
    parameters={'type': 'object',
     'properties': {'materialId': {'type': 'string',
                                   'minLength': 1,
                                   'maxLength': 64,
                                   'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'},
                    'anchor': {'type': 'object',
                               'properties': {'kind': {'type': 'string',
                                                       'minLength': 1,
                                                       'maxLength': 4,
                                                       'enum': ['page', 'time']},
                                              'offset': {'type': 'integer',
                                                         'minimum': 0,
                                                         'maximum': 86400000}},
                               'required': ['kind', 'offset'],
                               'additionalProperties': False}},
     'required': ['materialId', 'anchor'],
     'additionalProperties': False},
    preconditions=(Precondition.AUTHORIZED_ACTOR,),
    timeout_ms=1500,
    idempotency_key=None,
    version=1,
    description='Return one located material excerpt, at most 4KB; never reads an entire attachment.',
)
