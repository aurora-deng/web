"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import SkillSpec, Effect, Precondition

SPEC = SkillSpec(
    name='fleet.robot.quarantine',
    effect=Effect.PROPOSAL,
    parameters={'type': 'object',
     'properties': {'robotId': {'type': 'string',
                                'minLength': 1,
                                'maxLength': 64,
                                'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'},
                    'reason': {'type': 'string', 'minLength': 1, 'maxLength': 256}},
     'required': ['robotId', 'reason'],
     'additionalProperties': False},
    preconditions=(Precondition.AUTHORIZED_ACTOR, Precondition.HUMAN_APPROVAL,),
    timeout_ms=1000,
    idempotency_key=None,
    version=1,
    description='Record an operator quarantine request only; no robot control port is exposed.',
)
