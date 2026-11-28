"""Utility migration contracts; never run benchmark/database/process actions."""
import importlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from layout_evidence import matches_text_digest

spec = importlib.util.spec_from_file_location(
    'utility_migration', ROOT / 'scripts/project/layout/migrate_script_utilities_layout.py')
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)
BASH = os.environ.get('NEBULA_TEST_BASH') or shutil.which('bash')
LUA = shutil.which('lua') or shutil.which('luajit')


class ScriptUtilitiesLayout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = json.loads((ROOT / migration.MANIFEST).read_text(encoding='utf-8'))

    def test_exact_nine_moves_and_all_stable_entries(self):
        self.assertEqual(len(migration.MOVES), 9)
        self.assertEqual(self.manifest['moves'], [list(row) for row in migration.MOVES])
        self.assertFalse(self.manifest['behaviorChangeIntended'])
        self.assertFalse(self.manifest['historicalEvidenceRewritten'])
        self.assertEqual(self.manifest['compatibilityEntrypoints'],
                         [old for old, _ in migration.MOVES])
        for old, new in migration.MOVES:
            self.assertTrue((ROOT / old).is_file(), old)
            self.assertTrue((ROOT / new).is_file(), new)

    def test_implementation_and_wrapper_evidence(self):
        targets = {new for _, new in migration.MOVES}
        for row in self.manifest['files']:
            if row['new'] in targets:
                self.assertTrue(matches_text_digest((ROOT / row['new']).read_bytes(),
                                                    row['afterSha256']), row['new'])
        for row in self.manifest['generated']:
            self.assertTrue(matches_text_digest((ROOT / row['path']).read_bytes(),
                                                row['sha256']), row['path'])

    def test_python_ast_and_import_identity(self):
        for old, new in migration.MOVES:
            if not old.endswith('.py'):
                continue
            with self.subTest(path=old):
                self.assertEqual(migration.ast_digest((ROOT / new).read_text(encoding='utf-8')),
                                 self.manifest['pythonSemanticSha256'][new])
                canonical = importlib.import_module(new[:-3].replace('/', '.'))
                entry = importlib.util.spec_from_file_location('legacy_' + Path(old).stem, ROOT / old)
                legacy = importlib.util.module_from_spec(entry)
                entry.loader.exec_module(legacy)
                self.assertIs(legacy._implementation, canonical)
                self.assertIs(legacy.main, canonical.main)

    def test_python_legacy_main_forwards_without_running_real_action(self):
        import runpy
        for old, new in migration.MOVES:
            if not old.endswith('.py'):
                continue
            module = importlib.import_module(new[:-3].replace('/', '.'))
            with patch.object(module, 'main', return_value=29) as main:
                with patch.object(sys, 'argv', [old, 'argument with spaces', '']):
                    with self.assertRaises(SystemExit) as raised:
                        runpy.run_path(str(ROOT / old), run_name='__main__')
                self.assertEqual(raised.exception.code, 29)
                main.assert_called_once_with()

    def test_current_references_and_root_calculations(self):
        checks = {
            'scripts/benchmarks/benchmark.sh': ['scripts/reports/wrk_report.lua',
                                               'dirname "${BASH_SOURCE[0]}")/../..'],
            'scripts/reports/gen_report.sh': ['scripts/testing/run_tests.sh',
                                              'scripts/benchmarks/benchmark.sh',
                                              'dirname "${BASH_SOURCE[0]}")/../..'],
            'scripts/testing/run_tests.sh': ['dirname "${BASH_SOURCE[0]}")/../..'],
            'scripts/benchmarks/phase15-business-benchmark.sh':
                ['dirname -- "${BASH_SOURCE[0]}")/../..'],
            'scripts/benchmarks/phase26-capacity-benchmark.sh':
                ['exec "${BASH:-bash}" "${project_root}/scripts/benchmarks/phase15-business-benchmark.sh"'],
            'tests/integration/http_blackbox.py':
                ['root / "scripts" / "diagnostics" / "free_port_8080.py"'],
        }
        for name, needles in checks.items():
            text = (ROOT / name).read_text(encoding='utf-8')
            for needle in needles:
                self.assertIn(needle, text, name)
        # Phase 10 intentionally keeps its caller-relative BUILD_DIR/RESULT_ROOT semantics.
        phase10 = (ROOT / 'scripts/benchmarks/benchmark_phase10.sh').read_text(encoding='utf-8')
        self.assertIn('BUILD_DIR="${BUILD_DIR:-build-release}"', phase10)

    def test_replay_rejected_before_any_write(self):
        with self.assertRaisesRegex(RuntimeError, 'already applied'):
            migration.plan(ROOT)

    @unittest.skipUnless(BASH, 'Bash unavailable; shell runtime checks not executed')
    def test_shell_syntax_only(self):
        for old, new in migration.MOVES:
            if old.endswith('.sh'):
                for name in (old, new):
                    result = subprocess.run([BASH, '-n', (ROOT / name).as_posix()],
                                            capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, name + ': ' + result.stderr)

    @unittest.skipUnless(BASH, 'Bash unavailable; forwarding fixtures not executed')
    def test_shell_forwarding_uses_only_inert_fixture_targets(self):
        # Never execute a real operational implementation: each target is a printf-only stub.
        with tempfile.TemporaryDirectory(prefix='utility forwarding ') as temporary:
            root = Path(temporary)
            outside = root / 'unrelated cwd'
            outside.mkdir()
            env = dict(os.environ, NEBULA_FIXTURE_VALUE='kept with spaces')
            env['PATH'] = str(Path(BASH).parent) + os.pathsep + env.get('PATH', '')
            for old, new in migration.MOVES:
                if not old.endswith('.sh'):
                    continue
                entry, target = root / old, root / new
                entry.parent.mkdir(parents=True, exist_ok=True)
                target.parent.mkdir(parents=True, exist_ok=True)
                entry.write_text((ROOT / old).read_text(encoding='utf-8'), encoding='utf-8')
                target.write_text('#!/usr/bin/env bash\nprintf "<%s>\\n" "$#" "$@" "${NEBULA_FIXTURE_VALUE}"\nexit 23\n',
                                  encoding='utf-8')
                args = ['argument with spaces', '', '*literal$']
                result = subprocess.run([BASH, entry.as_posix(), *args], cwd=outside,
                                        env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 23, result.stderr)
                self.assertEqual(result.stdout.splitlines(),
                                 ['<3>', '<argument with spaces>', '<>', '<*literal$>',
                                  '<kept with spaces>'])

    @unittest.skipUnless(BASH, 'Bash unavailable; root-expression fixtures not executed')
    def test_shell_root_expressions_from_arbitrary_cwd(self):
        with tempfile.TemporaryDirectory(prefix='utility roots ') as temporary:
            root = Path(temporary)
            outside = root / 'unrelated cwd'
            outside.mkdir()
            env = dict(os.environ)
            env.pop('ROOT_DIR', None)
            env['PATH'] = str(Path(BASH).parent) + os.pathsep + env.get('PATH', '')
            for _, new in migration.MOVES:
                if not new.endswith('.sh'):
                    continue
                text = (ROOT / new).read_text(encoding='utf-8')
                assignments = [line for line in text.splitlines()
                               if line.startswith(('ROOT_DIR=', 'root_dir=', 'project_root='))]
                if not assignments:
                    continue
                self.assertEqual(len(assignments), 1)
                variable = assignments[0].split('=', 1)[0]
                probe = root / new
                probe.parent.mkdir(parents=True, exist_ok=True)
                probe.write_text(assignments[0] + '\nprintf "%s\\n" "${' + variable + '}"\n',
                                 encoding='utf-8')
                result = subprocess.run([BASH, probe.as_posix()], cwd=outside, env=env,
                                        capture_output=True, text=True, timeout=10)
                # Ask the same shell for the expected representation (MSYS uses POSIX paths).
                expected = subprocess.run([BASH, '-c', 'pwd'], cwd=root, env=env,
                                          capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, expected.stdout)

    @unittest.skipUnless(LUA, 'Lua unavailable; compatibility loader not runtime-verified')
    def test_lua_loader_with_inert_target_from_arbitrary_cwd(self):
        with tempfile.TemporaryDirectory(prefix='utility lua ') as temporary:
            root = Path(temporary)
            entry = root / 'scripts/wrk_report.lua'
            target = root / 'scripts/reports/wrk_report.lua'
            target.parent.mkdir(parents=True)
            entry.write_text((ROOT / 'scripts/wrk_report.lua').read_text(encoding='utf-8'),
                             encoding='utf-8')
            target.write_text('io.write("fixture-loaded\\n")\n', encoding='utf-8')
            result = subprocess.run([LUA, entry.as_posix()], cwd=root,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, 'fixture-loaded\n')


if __name__ == '__main__':
    unittest.main()
