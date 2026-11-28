"""Move nine root utilities by responsibility; keep stable compatibility CLIs.

The default only plans exact allowlisted files. Apply requires a reviewed digest
and verified new backup; it never runs the moved operational scripts.
"""
import argparse
import ast
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    'script_utilities_safety', Path(__file__).with_name('migrate_application_layout.py'))
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)
MANIFEST = 'docs/architecture/source-script-utilities-migration.json'
MOVES = [
    ('scripts/benchmark.sh', 'scripts/benchmarks/benchmark.sh'),
    ('scripts/benchmark_phase10.sh', 'scripts/benchmarks/benchmark_phase10.sh'),
    ('scripts/compare_phase10_results.py', 'scripts/benchmarks/compare_phase10_results.py'),
    ('scripts/phase15-business-benchmark.sh', 'scripts/benchmarks/phase15-business-benchmark.sh'),
    ('scripts/phase26-capacity-benchmark.sh', 'scripts/benchmarks/phase26-capacity-benchmark.sh'),
    ('scripts/gen_report.sh', 'scripts/reports/gen_report.sh'),
    ('scripts/wrk_report.lua', 'scripts/reports/wrk_report.lua'),
    ('scripts/run_tests.sh', 'scripts/testing/run_tests.sh'),
    ('scripts/free_port_8080.py', 'scripts/diagnostics/free_port_8080.py'),
]
REFERENCE_REWRITES = {
    'tests/integration/http_blackbox.py': {
        'root / "scripts" / "free_port_8080.py"':
        'root / "scripts" / "diagnostics" / "free_port_8080.py"',
    },
    'tests/project/test_phase11_operations_layout.py': {
        "'scripts/phase15-business-benchmark.sh':'scripts/function3/runtime/webserver/service.sh'":
        "'scripts/benchmarks/phase15-business-benchmark.sh':'scripts/function3/runtime/webserver/service.sh'",
    },
}


def ast_digest(text):
    return safety.sha(ast.dump(ast.parse(text), include_attributes=False).encode('utf-8'))


def rewrite_content(text, old):
    if old in ('scripts/benchmark.sh', 'scripts/gen_report.sh', 'scripts/run_tests.sh'):
        needle = 'dirname "${BASH_SOURCE[0]}")/..'
        if text.count(needle) != 1:
            raise RuntimeError('Expected exactly one root expression: ' + old)
        text = text.replace(needle, 'dirname "${BASH_SOURCE[0]}")/../..')
    if old in ('scripts/phase15-business-benchmark.sh', 'scripts/phase26-capacity-benchmark.sh'):
        needle = 'dirname -- "${BASH_SOURCE[0]}")/..'
        if text.count(needle) != 1:
            raise RuntimeError('Expected exactly one root expression: ' + old)
        text = text.replace(needle, 'dirname -- "${BASH_SOURCE[0]}")/../..')
    if old.endswith('.sh'):
        for before, after in MOVES:
            text = text.replace(before, after)
    if old == 'scripts/phase26-capacity-benchmark.sh':
        text = text.replace('exec "${project_root}/scripts/benchmarks/phase15-business-benchmark.sh"',
                            'exec "${BASH:-bash}" "${project_root}/scripts/benchmarks/phase15-business-benchmark.sh"')
    return text


def compatibility(old, new):
    if old.endswith('.py'):
        module = new[:-3].replace('/', '.')
        return f'''#!/usr/bin/env python3
"""Compatibility entrypoint; implementation: {module}."""
from pathlib import Path
import sys
from importlib import import_module
ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path: sys.path.insert(0, str(ROOT))
_implementation = import_module({module!r})
for _name in dir(_implementation):
    if not _name.startswith("__"): globals()[_name] = getattr(_implementation, _name)
if __name__ == "__main__":
    raise SystemExit(_implementation.main())
'''
    if old.endswith('.lua'):
        return '''-- Compatibility entrypoint; implementation: scripts/reports/wrk_report.lua
local script_path = debug.getinfo(1, "S").source:sub(2)
local script_dir = script_path:match("^(.*[/\\\\])") or "./"
dofile(script_dir .. "reports/wrk_report.lua")
'''
    return f'''#!/usr/bin/env bash
# Compatibility entrypoint; implementation: {new}
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${{BASH_SOURCE[0]}}")" && pwd)"
exec "${{BASH:-bash}}" "${{script_dir}}/{new.removeprefix('scripts/')}" "$@"
'''


def plan(root):
    if safety.bounded(root, MANIFEST).exists():
        raise RuntimeError('Script utilities migration already applied; do not replay')
    files, generated, semantic = {}, {}, {}
    for old, new in MOVES:
        path = safety.bounded(root, old)
        if not path.is_file() or safety.bounded(root, new).exists():
            raise RuntimeError('Missing source or occupied destination: ' + old)
        before = path.read_bytes()
        original = before.decode('utf-8').replace('\r\n', '\n')
        text = rewrite_content(original, old)
        if old.endswith('.py'):
            if ast_digest(original) != ast_digest(text):
                raise RuntimeError('Python behavior changed: ' + old)
            semantic[new] = ast_digest(text)
        files[old] = {'before': before, 'after': text.encode('utf-8'), 'destination': new}
        generated[old] = compatibility(old, new).encode('utf-8')
    for name, rewrites in REFERENCE_REWRITES.items():
        before = safety.bounded(root, name).read_bytes()
        text = before.decode('utf-8')
        for old, new in rewrites.items():
            if text.count(old) != 1:
                raise RuntimeError('Reference is missing or ambiguous: ' + name)
            text = text.replace(old, new)
        files[name] = {'before': before, 'after': text.encode('utf-8'), 'destination': name}
    readme = 'scripts/README.md'
    before = safety.bounded(root, readme).read_bytes()
    text = before.decode('utf-8')
    for old, new in MOVES:
        text = text.replace(old, new)
        text = text.replace('`' + old.removeprefix('scripts/') + '`',
                            '`' + new.removeprefix('scripts/') + '`')
    files[readme] = {'before': before, 'after': text.encode('utf-8'), 'destination': readme}
    document = {
        'schema': 'script-utilities-layout-v1', 'date': '2026-11-28',
        'moves': MOVES, 'behaviorChangeIntended': False,
        'pythonSemanticSha256': semantic,
        'compatibilityEntrypoints': [old for old, _ in MOVES],
        'referenceRewrites': REFERENCE_REWRITES,
        'historicalEvidenceRewritten': False,
        'files': [{'old': name, 'new': entry['destination'],
                   'beforeSha256': safety.sha(entry['before']),
                   'afterSha256': safety.sha(entry['after'])}
                  for name, entry in sorted(files.items())],
        'generated': [{'path': name, 'sha256': safety.sha(data)}
                      for name, data in sorted(generated.items())],
    }
    return files, generated, document


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('--backup', type=Path)
    parser.add_argument('--expected-plan-sha256')
    args = parser.parse_args()
    files, generated, document = plan(ROOT)
    digest = safety.sha(json.dumps(document, sort_keys=True).encode())
    print(json.dumps({'planSha256': digest, 'moves': len(MOVES),
                      'modifiedReferences': len(files) - len(MOVES)}))
    if args.apply:
        if args.backup is None:
            parser.error('--apply requires --backup')
        safety.apply(ROOT, args.backup, args.expected_plan_sha256, files, generated,
                     document, moves=MOVES, manifest=MANIFEST)


if __name__ == '__main__':
    main()
