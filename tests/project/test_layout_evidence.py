"""LF/CRLF compatibility without relaxing migration content integrity."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import unittest
from layout_evidence import matches_text_digest, text_variants

ROOT = Path(__file__).resolve().parents[2]


class LayoutEvidence(unittest.TestCase):
    def test_uniform_lf_and_crlf_match_the_same_recorded_text(self):
        lf = '第一行\nsecond line\n'.encode('utf-8')
        crlf = lf.replace(b'\n', b'\r\n')
        for current, recorded in ((lf, crlf), (crlf, lf), (lf, lf), (crlf, crlf)):
            with self.subTest(current=current):
                self.assertTrue(matches_text_digest(current, hashlib.sha256(recorded).hexdigest()))

    def test_content_changes_and_whitespace_are_not_ignored(self):
        data = b'original\nsecond\n'
        expected = hashlib.sha256(data).hexdigest()
        for changed in (b'changed\nsecond\n', b'original \nsecond\n', b'original\nsecond', b'original\rsecond\n'):
            with self.subTest(changed=changed):
                self.assertFalse(matches_text_digest(changed, expected))

    def test_mixed_line_endings_binary_and_non_utf8_are_not_normalized(self):
        for data in (b'a\r\nb\n', b'\0a\r\n', b'\xff\r\n', b'a\rb\n'):
            with self.subTest(data=data):
                self.assertEqual(list(text_variants(data)), [data])
                changed = data.replace(b'\r\n', b'\n')
                if changed != data:
                    self.assertFalse(matches_text_digest(data, hashlib.sha256(changed).hexdigest()))

    def test_invalid_digest_is_rejected(self):
        for value in ('', 'not-a-hash', 'A' * 64, None):
            self.assertFalse(matches_text_digest(b'data', value))

    def test_actual_git_blobs_match_old_ai_migration_evidence(self):
        manifest = json.loads((ROOT / 'docs/architecture/source-ai-script-migration.json').read_text(encoding='utf-8'))
        for row in manifest['entries']:
            with self.subTest(path=row['new']):
                blob = subprocess.check_output(['git', 'show', 'HEAD:' + row['new']], cwd=ROOT)
                self.assertTrue(matches_text_digest(blob, row['implementationSha256']))

    def test_cpp_semantic_evidence_accepts_git_checkout_line_endings_only(self):
        spec = importlib.util.spec_from_file_location('observability_evidence_test', ROOT / 'scripts/project/layout/migrate_application_observability_layout.py')
        migration = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(migration)
        manifest = json.loads((ROOT / migration.MANIFEST).read_text(encoding='utf-8'))
        for name, expected in manifest['semanticSha256'].items():
            with self.subTest(path=name):
                blob = subprocess.check_output(['git', 'show', 'HEAD:' + name], cwd=ROOT)
                digest = lambda data: migration.semantic_digest(data.decode('utf-8'))
                self.assertTrue(matches_text_digest(blob, expected, digest))
                self.assertFalse(matches_text_digest(blob + b'// changed behavior evidence\n', expected, digest))


if __name__ == '__main__':
    unittest.main()