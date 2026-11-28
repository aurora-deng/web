"""Version 1 contract; admission, effect and bounds remain in tool_runtime."""
from backend.agent.tool_runtime.registry import SkillSpec, Effect, Precondition

SPEC = SkillSpec(
    name='study.search_material',
    effect=Effect.READ,
    parameters={'type': 'object',
     'properties': {'query': {'type': 'string', 'minLength': 1, 'maxLength': 256},
                    'subject': {'type': 'string',
                                'minLength': 1,
                                'maxLength': 64,
                                'pattern': '[A-Za-z0-9][A-Za-z0-9_-]{0,63}'},
                    'type': {'type': 'string',
                             'minLength': 1,
                             'maxLength': 8,
                             'enum': ['note', 'pdf', 'video', 'document']},
                    'limit': {'type': 'integer', 'minimum': 1, 'maximum': 20}},
     'required': ['query', 'subject', 'type', 'limit'],
     'additionalProperties': False},
    preconditions=(Precondition.AUTHORIZED_ACTOR,),
    timeout_ms=2500,
    idempotency_key=None,
    version=1,
    description=('Search indexed metadata for one subject and material type, limit at most 20; never scans '
     'attachment bytes.'),
)
