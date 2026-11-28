"""Phase 11 compatibility and Function3 operations layout checks."""
import ast
import importlib
import importlib.util
import json
from pathlib import Path
import sys
import unittest

ROOT=Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0,str(ROOT))
spec=importlib.util.spec_from_file_location('phase11_operations_migration',ROOT/'scripts/project/layout/migrate_phase11_operations_layout.py')
migration=importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)


class Phase11OperationsLayout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest=json.loads((ROOT/migration.MANIFEST).read_text(encoding='utf-8'))

    def test_moved_implementations_and_compatibility_entries_are_complete(self):
        self.assertEqual(self.manifest['moves'],[list(move) for move in migration.MOVES])
        self.assertEqual(len(migration.MOVES),8)
        self.assertEqual(len(self.manifest['compatibilityEntrypoints']),8)
        for old,new in migration.MOVES:
            self.assertTrue((ROOT/new).is_file(),new)
            self.assertTrue((ROOT/old).is_file(),old)
        self.assertTrue((ROOT/'scripts/phase11/README.md').is_file())

    def test_python_implementations_keep_ast_and_import_identity(self):
        for old,new in migration.MOVES:
            if not old.endswith('.py'):
                continue
            with self.subTest(module=new):
                before=migration.module_ast_digest((ROOT/new).read_text(encoding='utf-8'))
                self.assertEqual(before,self.manifest['pythonSemanticSha256'][new])
                legacy_spec=importlib.util.spec_from_file_location('legacy_phase11_'+Path(old).stem.replace('-','_'),ROOT/old)
                legacy=importlib.util.module_from_spec(legacy_spec)
                legacy_spec.loader.exec_module(legacy)
                canonical=importlib.import_module(new[:-3].replace('/','.'))
                self.assertIs(legacy._implementation,canonical)
                self.assertIs(legacy.main,canonical.main)

    def test_shell_path_roots_and_supervisor_default_are_correct(self):
        migrate=(ROOT/'scripts/function3/database/migrate.sh').read_text(encoding='utf-8')
        status=(ROOT/'scripts/function3/database/migration_status.sh').read_text(encoding='utf-8')
        service=(ROOT/'scripts/function3/runtime/webserver/service.sh').read_text(encoding='utf-8')
        supervisor=(ROOT/'scripts/function3/runtime/webserver/supervisor.sh').read_text(encoding='utf-8')
        self.assertIn('dirname "${BASH_SOURCE[0]}")/../../..',migrate)
        self.assertIn('dirname "${BASH_SOURCE[0]}")/../../..',status)
        self.assertIn('${script_dir}/../../../..',service)
        self.assertIn('${script_dir}/service.sh',supervisor)

    def test_active_code_uses_canonical_paths(self):
        checks={
            'CMakeLists.txt':'scripts/function3/runtime/webserver/supervisor.sh',
            'scripts/function3/runtime/service.py':'scripts/function3/database/user_service.sh',
            'scripts/benchmarks/phase15-business-benchmark.sh':'scripts/function3/runtime/webserver/service.sh',
            'scripts/README.md':'scripts/function3/data/seed_demo_users.py',
        }
        for name,needle in checks.items():
            self.assertIn(needle,(ROOT/name).read_text(encoding='utf-8'))

    def test_old_python_entries_are_thin_and_migration_replay_fails(self):
        for old,new in migration.MOVES:
            if not old.endswith('.py'):
                continue
            tree=ast.parse((ROOT/old).read_text(encoding='utf-8'))
            self.assertFalse(any(isinstance(node,(ast.FunctionDef,ast.ClassDef)) for node in tree.body))
        with self.assertRaisesRegex(RuntimeError,'already applied'):
            migration.plan(ROOT,'unused')

    def test_no_credential_or_database_file_is_in_manifest(self):
        text=json.dumps(self.manifest)
        for marker in ('.env','.key','.pem','.sqlite','.db','password'):
            self.assertNotIn(marker,text.lower())


if __name__=='__main__':
    unittest.main()
