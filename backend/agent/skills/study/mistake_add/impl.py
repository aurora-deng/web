"""Deterministic recipe; authorization and audit stay in SkillRuntime."""


def run(runtime, p, call, *_):
    return call('mistake.write', p)
