"""Observer path, import and executable-AST parity; no listeners or DB writes."""
import ast
import hashlib
import importlib
import importlib.util
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
spec = importlib.util.spec_from_file_location('observer_layout_migration', ROOT / 'scripts/project/layout/migrate_observer_layout.py')
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)


def catalog_before_followup(text):
    """Reverse only the recorded catalog path move; keep original AST evidence."""
    manifest = json.loads((ROOT / 'docs/architecture/source-application-observability-migration.json').read_text(encoding='utf-8'))
    old = '/'.join(('backend/application/observability', 'OperationCatalog', 'OperationCatalog.def'))
    new = 'backend/application/observability/catalog/OperationCatalog.def'
    module = 'tools/observability/observer/contracts/operation_catalog.py'
    if manifest['moves'].count([old, new]) != 1:
        raise AssertionError('Unrecorded catalog path migration')
    records = {(entry['old'], entry['new']): entry for entry in manifest['files']}
    target = records[(old, new)]
    digest = hashlib.sha256((ROOT / new).read_bytes()).hexdigest()
    if digest != target['beforeSha256'] or digest != target['afterSha256']:
        raise AssertionError('Catalog contents changed during path migration')
    record = records[(module, module)]
    if text.count(new) != 1 or old in text:
        raise AssertionError('Unexpected catalog path')
    if hashlib.sha256(text.encode('utf-8')).hexdigest() != record['afterSha256']:
        raise AssertionError('Catalog reader differs from recorded migration')
    original = text.replace(new, old)
    if hashlib.sha256(original.encode('utf-8')).hexdigest() != record['beforeSha256']:
        raise AssertionError('Catalog migration changed more than its path')
    return original


class ObserverLayout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = json.loads((ROOT / migration.MANIFEST).read_text(encoding='utf-8'))

    def test_all_components_have_one_responsibility_home(self):
        self.assertEqual(self.manifest['moves'], [list(move) for move in migration.MOVES])
        self.assertEqual(len(self.manifest['semanticSha256']), len(migration.MOVES))
        for old, new in migration.MOVES:
            with self.subTest(module=new):
                self.assertTrue((ROOT / new).is_file())
                if old not in self.manifest['compatibilityEntrypoints']:
                    self.assertFalse((ROOT / old).exists())

    def test_moved_code_behavior_ast_is_unchanged(self):
        for name, expected in self.manifest['semanticSha256'].items():
            with self.subTest(module=name):
                text = (ROOT / name).read_bytes().decode('utf-8')
                if name == 'tools/observability/observer/contracts/operation_catalog.py':
                    text = catalog_before_followup(text)
                self.assertEqual(migration.semantic_digest(text), expected)

    def test_catalog_followup_rejects_unknown_paths_and_behavior_changes(self):
        text = (ROOT / 'tools/observability/observer/contracts/operation_catalog.py').read_bytes().decode('utf-8')
        for changed in (text.replace('observability/catalog/', 'observability/unknown/'),
                        text + '\nraise RuntimeError("unexpected change")\n'):
            with self.subTest(changed=changed[-60:]), self.assertRaises(AssertionError):
                catalog_before_followup(changed)

    def test_moved_modules_import_without_running_service_entrypoints(self):
        for _, new in migration.MOVES:
            with self.subTest(module=new):
                importlib.import_module(new[:-3].replace('/', '.'))

    def test_checkout_roots_resolve_to_original_source(self):
        for name in ['contracts.operation_catalog', 'http.server']:
            module = importlib.import_module('tools.observability.observer.' + name)
            self.assertEqual(module.ROOT, ROOT)

    def test_legacy_server_cli_is_thin_and_uses_same_server_class(self):
        old = importlib.import_module('tools.observability.observer.server')
        current = importlib.import_module('tools.observability.observer.http.server')
        self.assertIs(old.ObserverServer, current.ObserverServer)
        self.assertIs(old.main, current.main)
        tree = ast.parse((ROOT / migration.PREFIX / 'server.py').read_text(encoding='utf-8'))
        self.assertFalse(any(isinstance(node, (ast.FunctionDef, ast.ClassDef)) for node in tree.body))

    def test_responsibility_packages_have_no_startup_side_effects(self):
        for group in migration.GROUPS:
            path = ROOT / migration.PREFIX / group / '__init__.py'
            tree = ast.parse(path.read_text(encoding='utf-8'))
            self.assertEqual(len(tree.body), 1)
            self.assertIsInstance(tree.body[0], ast.Expr)
            self.assertIsInstance(tree.body[0].value, ast.Constant)

    def test_migration_replay_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'already applied'):
            migration.plan(ROOT, 'unused-git')

    def test_import_transform_handles_relative_imports_bom_and_module_alias(self):
        output = migration.import_rewrite('\ufefffrom .events import Invalid\n', migration.PREFIX + '/server.py')
        self.assertEqual(output, '\ufefffrom tools.observability.observer.contracts.events import Invalid\n')
        output = migration.import_rewrite('from . import service_levels\n', migration.PREFIX + '/prometheus_service_levels.py')
        self.assertEqual(output, 'from tools.observability.observer.metrics import service_levels\n')
        output = migration.import_rewrite('from tools.observability.observer.sources import PANELS\n', 'tests/project/example.py')
        self.assertEqual(output, 'from tools.observability.observer.sources.sources import PANELS\n')
        self.assertNotIn('.sources.sources.sources', output)


if __name__ == '__main__':
    unittest.main()
