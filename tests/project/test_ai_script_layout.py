"""Layout checks use source bytes, mocked imports and synthetic model files only."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from layout_evidence import matches_text_digest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


class AiScriptLayout(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads((ROOT / 'docs/architecture/source-ai-script-migration.json').read_text(encoding='utf-8'))

    def test_implementation_bytes_match_migration_manifest(self):
        self.assertEqual(len(self.manifest['entries']), 11)
        for entry in self.manifest['entries']:
            with self.subTest(path=entry['new']):
                content = (ROOT / entry['new']).read_bytes()
                self.assertTrue(matches_text_digest(content, entry['implementationSha256']), entry['new'])
                if entry['new'].endswith('.py'):
                    compile(content, entry['new'], 'exec')
                if entry['adjustment'] == 'byte-identical':
                    self.assertEqual(entry['beforeSha256'], entry['implementationSha256'])

    def test_python_compatibility_paths_and_exports(self):
        for entry in self.manifest['entries']:
            if entry['compatibility'] != 'python-entrypoint':
                continue
            expected = '.'.join(Path(entry['new']).with_suffix('').parts)
            sentinel = object()
            fake = SimpleNamespace(main=lambda: 0, _helper=sentinel)
            with self.subTest(path=entry['old']), patch('importlib.import_module', return_value=fake) as importer:
                spec = importlib.util.spec_from_file_location('layout_compat_fixture', ROOT / entry['old'])
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                importer.assert_called_once_with(expected)
                self.assertIs(module.main, fake.main)
                self.assertIs(module._helper, sentinel)
                self.assertEqual(module.ROOT, ROOT)

    def test_constraint_paths_are_unchanged_and_install_guard_preserved(self):
        for entry in self.manifest['entries']:
            if entry['compatibility'] == 'constraint-copy':
                self.assertEqual((ROOT / entry['old']).read_bytes(), (ROOT / entry['new']).read_bytes())
        script = (ROOT / 'scripts/ai/toolchain/install-lora-toolchain.sh').read_text(encoding='utf-8')
        self.assertLess(script.index('"--install"'), script.index('pip install'))
        old = (ROOT / 'scripts/phase11/install-lora-toolchain.sh').read_text(encoding='utf-8')
        self.assertIn('exec bash "${root}/scripts/ai/toolchain/install-lora-toolchain.sh" "$@"', old)

    def test_checksum_cli_compatibility_with_synthetic_files(self):
        base = ROOT / 'build-function3/ai-script-layout'
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temp:
            root = Path(temp)
            (root / 'synthetic-model.bin').write_bytes(b'not a real model')
            commands = ['scripts/phase11/model_checksum.py', 'scripts/ai/export/model_checksum.py']
            values = [subprocess.check_output([sys.executable, str(ROOT / script), str(root)], text=True).strip()
                      for script in commands]
            self.assertEqual(values[0], values[1])
            self.assertTrue(values[0].startswith('sha256-tree-v1:'))


if __name__ == '__main__':
    unittest.main()
