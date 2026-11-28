"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import SkillSpec, Effect, Precondition

SPEC = SkillSpec(
    name='workflow.run.trace',
    effect=Effect.READ,
    parameters={'type': 'object',
     'properties': {'runId': {'type': 'string',
                              'minLength': 1,
                              'maxLength': 64,
                              'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'},
                    'nodeLimit': {'type': 'integer', 'minimum': 1, 'maximum': 64}},
     'required': ['runId', 'nodeLimit'],
     'additionalProperties': False},
    preconditions=(Precondition.AUTHORIZED_ACTOR,),
    timeout_ms=1000,
    idempotency_key=None,
    version=1,
    description='Read persisted checkpoints of one run, at most 64 nodes.',
)
