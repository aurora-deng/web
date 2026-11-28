"""Read-only AI layout/include contract, no build/network/runtime actions."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class AiCppLayout(unittest.TestCase):
    def test_all_fifteen_components_moved_without_old_duplicates(self):
        manifest = json.loads((ROOT / 'docs/architecture/source-layout-migration-2.json').read_text(encoding='utf-8'))
        self.assertEqual(len(manifest['moves']), 15)
        self.assertTrue(manifest['frozenCoreUnchanged'])
        self.assertFalse(manifest['historicalEvidenceRewritten'])
        for old, new in manifest['moves']:
            with self.subTest(component=new):
                self.assertFalse((ROOT / old).exists())
                self.assertTrue((ROOT / new).is_dir())

    def test_all_cpp_includes_resolve(self):
        for folder in ('backend/ai', 'backend/application'):
            for path in (ROOT / folder).rglob('*'):
                if path.suffix not in ('.h', '.cpp'):
                    continue
                for header in re.findall(r'#include\s*"(backend/[^"\n]+)"', path.read_text(encoding='utf-8')):
                    with self.subTest(source=str(path.relative_to(ROOT)), include=header):
                        self.assertTrue((ROOT / header).is_file())
                        self.assertFalse(header.startswith('backend/ai/models/'))

    def test_cmake_uses_new_component_paths(self):
        cmake = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        self.assertNotIn('backend/ai/models/', cmake)
        for path in re.findall(r'backend/ai/[^\s)]+\.cpp', cmake):
            self.assertTrue((ROOT / path).is_file(), path)


if __name__ == '__main__':
    unittest.main()
