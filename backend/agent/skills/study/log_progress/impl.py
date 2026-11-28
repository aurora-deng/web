"""Deterministic recipe; authorization and audit stay in SkillRuntime."""


def run(runtime, p, call, *_):
    return call('progress.write', p)
