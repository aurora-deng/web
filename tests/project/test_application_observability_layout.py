"""Application observability C++ responsibility layout checks."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import unittest
from layout_evidence import matches_text_digest

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('app_observability_migration',ROOT/'scripts/project/layout/migrate_application_observability_layout.py')
migration=importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)


class ApplicationObservabilityLayout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest=json.loads((ROOT/migration.MANIFEST).read_text(encoding='utf-8'))

    def test_components_are_in_responsibility_directories(self):
        self.assertEqual(self.manifest['moves'],[list(move) for move in migration.MOVES])
        self.assertEqual(len(migration.MOVES),15)
        self.assertEqual(len(self.manifest['semanticSha256']),15)
        for old,new in migration.MOVES:
            with self.subTest(component=new):
                self.assertFalse((ROOT/old).exists(),old)
                self.assertTrue((ROOT/new).is_file(),new)
                self.assertTrue(matches_text_digest((ROOT/new).read_bytes(),
                                self.manifest['semanticSha256'][new],
                                lambda value: migration.semantic_digest(value.decode('utf-8'))), new)

    def test_cmake_and_runtime_allowlist_use_canonical_paths(self):
        cmake=(ROOT/'CMakeLists.txt').read_text(encoding='utf-8')
        package=(ROOT/'scripts/project/package_release.py').read_text(encoding='utf-8')
        self.assertIn('backend/application/observability/tracing/Telemetry.cpp',cmake)
        self.assertIn('backend/application/observability/metrics/BusinessMetricsFormatter.cpp',cmake)
        self.assertIn('backend/application/observability/catalog',package)
        for old in migration.REWRITES:
            self.assertNotIn(old,cmake)

    def test_internal_references_have_no_old_component_paths(self):
        git='git'
        names=subprocess.check_output([git,'-c','core.fsmonitor=false','ls-files','--cached','--others','--exclude-standard','-z'],cwd=ROOT).decode('utf-8').split('\0')
        suffixes={'.h','.cpp','.def','.cmake','.txt','.md','.json','.py','.sh'}
        for name in names:
            if not name or name.startswith(('backend/server/','docs/evidence/','docs/archive/','docs/reports/','docs/performance/','docs/architecture/source-')):
                continue
            if migration.safety.private_source_name(name):
                continue
            if not name.endswith(tuple(suffixes)) and name!='CMakeLists.txt':
                continue
            path=ROOT/name
            if not path.is_file():
                continue
            text=path.read_text(encoding='utf-8')
            for old in migration.REWRITES:
                self.assertNotIn(old,text,f'{name} still references {old}')

    def test_frozen_server_has_no_diff(self):
        git='git'
        changed=subprocess.check_output([git,'diff','--name-only','--','backend/server'],cwd=ROOT,text=True).splitlines()
        self.assertEqual(changed,[])

    def test_migration_replay_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError,'already applied'):
            migration.plan(ROOT,'unused')


if __name__=='__main__':
    unittest.main()
