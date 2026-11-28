"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import SkillSpec, Effect, Precondition

SPEC = SkillSpec(
    name='fleet.propose_sim_reschedule',
    effect=Effect.PROPOSAL,
    parameters={'type': 'object',
     'properties': {'taskId': {'type': 'string',
                               'minLength': 1,
                               'maxLength': 64,
                               'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'}},
     'required': ['taskId'],
     'additionalProperties': False},
    preconditions=(Precondition.AUTHORIZED_ACTOR, Precondition.HUMAN_APPROVAL,),
    timeout_ms=2500,
    idempotency_key=None,
    version=1,
    description='Stage a replacement of one queued simulation task; approval uses the existing C++ review path.',
)
