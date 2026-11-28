"""Deterministic recipe; authorization and audit stay in SkillRuntime."""


def run(runtime, p, call, *_):
    return call('quiz.next', dict(p, excludeMastered=True))
