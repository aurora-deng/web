"""Monitoring tool layout, module identity and delivery checks; no lab startup."""
import ast
import importlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0,str(ROOT))
spec=importlib.util.spec_from_file_location('monitor_tool_migration',ROOT/'scripts/project/layout/migrate_observability_tools_layout.py')
migration=importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)


class ObservabilityToolsLayout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest=json.loads((ROOT/migration.MANIFEST).read_text(encoding='utf-8'))

    def test_responsibility_moves_are_complete_and_unique(self):
        self.assertEqual(self.manifest['moves'],[list(move) for move in migration.MOVES])
        self.assertEqual(len(migration.MOVES),23)
        self.assertEqual(len(set(new for _,new in migration.MOVES)),23)
        self.assertEqual(len(self.manifest['stableCommandEntrypoints']),20)
        self.assertEqual(len(self.manifest['semanticSha256']),23)
        for old,new in migration.MOVES:
            self.assertTrue((ROOT/new).is_file(),new)
            self.assertTrue((ROOT/old).is_file(),old)

    def test_executable_behavior_and_process_identity_literals_are_unchanged(self):
        for name,expected in self.manifest['semanticSha256'].items():
            with self.subTest(module=name):
                self.assertEqual(migration.semantic_digest((ROOT/name).read_text(encoding='utf-8')),expected)
        self.assertTrue(self.manifest['processIdentityLiteralsPreserved'])
        # Ownership checks and nested CLI calls intentionally keep these names.
        lab=(ROOT/'tools/observability/laboratory/lab.py').read_text(encoding='utf-8')
        retention=(ROOT/'tools/observability/lifecycle/retention.py').read_text(encoding='utf-8')
        self.assertIn('tools.observability.probe',lab)
        self.assertIn('tools.observability.lab',retention)

    def test_old_and_new_imports_have_identical_module_identity(self):
        for old,new in migration.MOVES:
            with self.subTest(module=old):
                legacy=importlib.import_module(old[:-3].replace('/','.'))
                canonical=importlib.import_module(new[:-3].replace('/','.'))
                self.assertIs(legacy,canonical)

    def test_legacy_mock_targets_still_patch_canonical_function_globals(self):
        old=importlib.import_module('tools.observability.lab')
        current=importlib.import_module('tools.observability.laboratory.lab')
        self.assertIs(old.private_write,current.private_write)
        sentinel=object()
        with patch('tools.observability.lab.os.replace',sentinel):
            self.assertIs(current.private_write.__globals__['os'].replace,sentinel)

    def test_checkout_roots_and_namespace_identity_stay_unchanged(self):
        for name in ('gateways.gateway','laboratory.lab','diagnostics.ack_diagnosis',
                     'diagnostics.experiments','laboratory.resource_lab','verification.verify','verification.verify_gateway'):
            module=importlib.import_module('tools.observability.'+name)
            self.assertEqual(module.ROOT,ROOT)
        # The installer destination is a provisioned dependency path, not the checkout root.
        installer=importlib.import_module('tools.observability.pipeline.install_metrics_backend')
        self.assertEqual(str(installer.ROOT).replace('\\','/'),'/home/pikachu/phase11-deps/observability')

    def test_compatibility_files_and_package_initializers_have_no_implementation(self):
        for old,_ in migration.MOVES:
            tree=ast.parse((ROOT/old).read_text(encoding='utf-8'))
            self.assertFalse(any(isinstance(node,(ast.FunctionDef,ast.ClassDef)) for node in ast.walk(tree)))
        for group in migration.GROUPS:
            tree=ast.parse((ROOT/migration.PREFIX/group/'__init__.py').read_text(encoding='utf-8'))
            self.assertEqual(len(tree.body),1)
            self.assertIsInstance(tree.body[0],ast.Expr)
            self.assertIsInstance(tree.body[0].value,ast.Constant)
            self.assertTrue((ROOT/migration.PREFIX/group/'README.md').is_file())

    def test_gateway_delivery_includes_implementation_but_not_installers(self):
        sys.path.insert(0,str(ROOT/'scripts/project'))
        from package_release import RUNTIME_ENTRY_FILES,runtime_path
        required={'tools/observability/gateway.py','tools/observability/gateways/__init__.py','tools/observability/gateways/gateway.py'}
        self.assertTrue(required<=RUNTIME_ENTRY_FILES)
        for name in required:
            self.assertTrue(runtime_path(name))
            self.assertTrue((ROOT/name).is_file())
        for name in ('tools/observability/pipeline/install_pipeline.py','tools/observability/laboratory/lab.py'):
            self.assertFalse(runtime_path(name))

    def test_released_gateway_imports_without_checkout(self):
        # A temporary source-only package proves that the shipped wrapper does
        # not import unshipped laboratory or installer code. No network calls.
        required=['tools/observability/gateway.py','tools/observability/gateways/__init__.py',
                  'tools/observability/gateways/gateway.py','tools/observability/observer/http/access_preflight.py',
                  'tools/observability/observer/http/__init__.py']
        with tempfile.TemporaryDirectory() as directory:
            for name in required:
                dest=Path(directory)/name
                dest.parent.mkdir(parents=True,exist_ok=True)
                dest.write_bytes((ROOT/name).read_bytes())
            result=subprocess.run([sys.executable,'-B','-c',
                'from tools.observability.gateway import render; assert callable(render)'],
                cwd=directory,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=15)
            self.assertEqual(result.returncode,0,result.stderr.decode('utf-8',errors='replace'))

    def test_migration_replay_is_rejected_before_git(self):
        with self.assertRaisesRegex(RuntimeError,'already applied'):
            migration.plan(ROOT,'unused')

    def test_transform_preserves_literals_and_handles_bom_unicode_and_aliases(self):
        old='tools/observability/lab.py'
        source='\ufefffrom . import gateway, lab_config\nOWNER="tools.observability.lab"\n'
        output=migration.import_rewrite(source,old)
        self.assertTrue(output.startswith('\ufefffrom tools.observability.gateways import gateway'))
        self.assertIn('from tools.observability.laboratory import lab_config',output)
        self.assertIn('OWNER="tools.observability.lab"',output)
        source='label="\u4e2d"; from .lab import ROOT as checkout\n'
        output=migration.import_rewrite(source,'tools/observability/alert_lab.py')
        self.assertEqual(output,'label="\u4e2d"; from tools.observability.laboratory.lab import ROOT as checkout\n')
        with self.assertRaisesRegex(RuntimeError,'bare module import'):
            migration.import_rewrite('import tools.observability.lab\n',old)


if __name__=='__main__':
    unittest.main()
