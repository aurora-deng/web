"""Offline application-domain layout and unchanged-business-contract checks."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import unittest
from layout_evidence import matches_text_digest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('application_layout_migration', ROOT / 'scripts/project/layout/migrate_application_layout.py')
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)
MANIFEST = ROOT / migration.MANIFEST


class ApplicationLayout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))

    def test_each_reviewed_component_has_one_new_home(self):
        self.assertEqual(self.manifest['moves'], [list(move) for move in migration.MOVES])
        self.assertEqual(len(set(old for old, _ in migration.MOVES)), len(migration.MOVES))
        self.assertEqual(len(set(new for _, new in migration.MOVES)), len(migration.MOVES))
        for old, new in migration.MOVES:
            with self.subTest(component=new):
                self.assertFalse((ROOT / old).exists(), old)
                self.assertTrue((ROOT / new).exists(), new)
                self.assertFalse(old.startswith('backend/server/'))
                self.assertFalse(new.startswith('backend/server/'))

    def test_business_interface_definitions_are_exactly_preserved(self):
        actual = {}
        for entry in self.manifest['generated']:
            text = (ROOT / entry['path']).read_text(encoding='utf-8')
            for definition, name in re.findall(r'^((?:class|struct) (\w+)\n\{.*?^\};)', text, re.M | re.S):
                self.assertNotIn(name, actual)
                actual[name] = hashlib.sha256(definition.encode('utf-8')).hexdigest()
        self.assertEqual(set(actual), set(migration.CONTRACTS))
        self.assertEqual(actual, self.manifest['contractDefinitions'])

    def test_existing_generated_artifacts_are_unchanged(self):
        for name, expected in self.manifest['preservedArtifacts'].items():
            with self.subTest(artifact=name):
                data = (ROOT / name).read_bytes()
                if name == 'docs/contracts/business-routes.json':
                    # Only source-location metadata follows moved C++ files.
                    text = data.decode('utf-8')
                    for old, new in sorted(migration.MOVES, key=lambda item: -len(item[1])):
                        text = text.replace(new + '/' if not Path(new).suffix else new,
                                            old + '/' if not Path(old).suffix else old)
                    data = text.encode('utf-8')
                self.assertTrue(matches_text_digest(data, expected), name)
        self.assertTrue(self.manifest['frozenCoreUnchanged'])
        self.assertFalse(self.manifest['historicalEvidenceRewritten'])
        self.assertFalse(self.manifest['behaviorChangeIntended'])

    def test_all_project_local_cpp_headers_resolve(self):
        for folder in ['backend/application', 'backend/ai', 'tools/ai']:
            for source in (ROOT / folder).rglob('*'):
                if source.suffix not in ['.h', '.cpp']:
                    continue
                for header in re.findall(r'^\s*#\s*include\s*"(backend/[^"\n]+)"', source.read_text(encoding='utf-8'), re.M):
                    with self.subTest(source=source.relative_to(ROOT), include=header):
                        self.assertTrue((ROOT / header).is_file(), header)

    def test_no_live_cpp_uses_old_migrated_component_paths(self):
        for folder in ['backend', 'tests', 'tools', 'examples', 'benchmarks']:
            for source in (ROOT / folder).rglob('*'):
                if source.suffix not in ['.h', '.cpp'] or source.is_relative_to(ROOT / 'backend/server'):
                    continue
                text = source.read_text(encoding='utf-8')
                for old, _ in migration.MOVES:
                    with self.subTest(source=source.relative_to(ROOT), old=old):
                        self.assertNotIn(old + '/' if not Path(old).suffix else old, text)

    def test_compatibility_umbrella_contains_includes_not_business_definitions(self):
        path = ROOT / migration.MODULES / 'shared/compatibility/BusinessInterfaces/BusinessInterfaces.h'
        text = path.read_text(encoding='utf-8')
        self.assertEqual(len(re.findall(r'^#include', text, re.M)), 7)
        self.assertNotRegex(text, r'(?m)^(?:struct|class) ')

    def test_cmake_source_entries_resolve(self):
        for source in [ROOT / 'CMakeLists.txt', ROOT / 'backend/application/modules/CMakeLists.txt']:
            for path in re.findall(r'(?<![\w/])((?:backend|tools)/[^\s"()]+\.cpp)', source.read_text(encoding='utf-8')):
                with self.subTest(cmake=source.name, path=path):
                    self.assertTrue((ROOT / path).is_file(), path)

    def test_legacy_business_and_http_paths_are_forwarding_headers_only(self):
        for folder in ['backend/application/business', 'backend/application/transport/http']:
            for path in (ROOT / folder).rglob('*'):
                if not path.is_file():
                    continue
                with self.subTest(path=path.relative_to(ROOT)):
                    self.assertEqual(path.suffix, '.h')
                    lines = path.read_text(encoding='utf-8').splitlines()
                    self.assertLessEqual(len(lines), 3)
                    self.assertIn('Compatibility include;', '\n'.join(lines))
                    self.assertRegex(lines[-1], r'^#include "backend/application/modules/')

    def test_no_infrastructure_headers_in_any_domain_or_port(self):
        for module in (ROOT / migration.MODULES).iterdir():
            if not module.is_dir():
                continue
            for layer in ['domain', 'ports']:
                for path in (module / layer).rglob('*.h'):
                    for header in re.findall(r'#include\s*[<"]([^>"\n]+)', path.read_text(encoding='utf-8')):
                        with self.subTest(path=path.relative_to(ROOT), header=header):
                            self.assertFalse(any(word in header for word in ['backend/server/', '/storage/', 'libpq', 'grpcpp', 'onnxruntime']))

    def test_private_and_generated_paths_are_excluded_before_file_reads(self):
        for name in ['scripts/auth.json', 'backend/secrets.json', 'tools/credentials.json',
                     'docs/account.local.json', '.env.production', 'backend/models/data.json',
                     'tools/__pycache__/module.py', 'build-release/generated.cpp', 'backend/private.key']:
            with self.subTest(path=name):
                self.assertTrue(migration.private_source_name(name))
        for name in ['scripts/project/build_stamp.py', 'backend/ai/registry/ModelRegistryService/ModelRegistryService.cpp']:
            self.assertFalse(migration.private_source_name(name))

    def test_migration_cannot_be_replayed(self):
        with self.assertRaisesRegex(RuntimeError, 'already applied'):
            migration.plan(ROOT, 'unused-git')

    def test_parent_and_absolute_paths_are_rejected(self):
        for name in ['../outside', str(ROOT.resolve()), '/outside']:
            with self.subTest(path=name), self.assertRaises(RuntimeError):
                migration.bounded(ROOT, name)

    def test_stale_digest_fails_before_backup_or_source_write(self):
        with self.assertRaisesRegex(RuntimeError, 'digest changed'):
            migration.apply(ROOT, ROOT.parent / 'must-not-be-created.tar.gz', 'stale', {}, {}, {})
        self.assertFalse((ROOT.parent / 'must-not-be-created.tar.gz').exists())

    def test_contract_split_refuses_an_unknown_definition_set(self):
        with self.assertRaisesRegex(RuntimeError, 'definition set changed'):
            migration.split_contracts('struct Unreviewed\n{\n};\n')


if __name__ == '__main__':
    unittest.main()
