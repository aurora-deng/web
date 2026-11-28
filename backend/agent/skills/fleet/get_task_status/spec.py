"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import SkillSpec, Effect, Precondition

SPEC = SkillSpec(
    name='fleet.get_task_status',
    effect=Effect.READ,
    parameters={'type': 'object',
     'properties': {'taskId': {'type': 'string',
                               'minLength': 1,
                               'maxLength': 64,
                               'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'}},
     'required': ['taskId'],
     'additionalProperties': False},
    preconditions=(Precondition.AUTHORIZED_ACTOR,),
    timeout_ms=1500,
    idempotency_key=None,
    version=1,
    description='Read one task status without mutation.',
)
