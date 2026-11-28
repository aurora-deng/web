"""Literal source-graph checks with isolated fixtures; no browser or network."""
from pathlib import Path
from html.parser import HTMLParser
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.project.frontend_graph import analyze, imports, local_reference


class FrontendGraph(unittest.TestCase):
    def test_static_and_dynamic_imports_and_comments(self):
        source = """// import './fake.js';
import { one,
 two } from './a.js?v=1';
export { three } from './b.js';
/* import './missing.js'; */
const later = import('./lazy.js?v=1');
"""
        self.assertEqual(imports(source), (["./a.js?v=1", "./b.js"], ["./lazy.js?v=1"]))

    def test_import_like_strings_and_templates_are_not_dependencies(self):
        source = 'const text = "import(\'./not-real.js\')";\nconst template = `\nimport \'./also-not-real.js\';\n`;\nexport * as alias from \'./actual.js\';'
        self.assertEqual(imports(source), (["./actual.js"], []))

    def fixture(self, root):
        front = root / "frontend/phase11"
        front.mkdir(parents=True)
        files = {"index.html": '<script type="module" src="/phase11/app.js?v=1"></script><link rel="stylesheet" href="/phase11/base.css">',
                 "app.js": "import './a.js?v=1';\nimport('./lazy.js?v=1');",
                 "a.js": "import './b.js';", "b.js": "import './app.js';", "lazy.js": "", "base.css": ""}
        for name, text in files.items(): (front / name).write_text(text, encoding="utf-8")
        return front / "index.html"

    def test_cycle_is_bounded_and_dynamic_module_is_not_eager(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            report = analyze(root, self.fixture(root))
            self.assertEqual(report["staticModules"], 3)
            self.assertEqual(report["staticDiscoveryLayers"], 3)
            self.assertEqual(report["literalDynamicEdges"], 1)
            self.assertFalse(any(p["path"].endswith("lazy.js") for p in report["modules"]))
            self.assertIn("/phase11/a.js?v=1", report["modulepreloadCandidates"])

    def test_real_entry_preloads_only_bounded_eager_same_origin_modules(self):
        from scripts.project.frontend_graph import EntryParser
        from urllib.parse import urlsplit
        entry = ROOT / "frontend/phase11/index.html"
        report = analyze(ROOT, entry)
        parser = EntryParser()
        parser.feed(entry.read_text(encoding="utf-8"))
        self.assertEqual(len(parser.preloads), 5)
        self.assertEqual(len(set(parser.preloads)), 5)
        urls = {module["url"]: module for module in report["modules"]}
        self.assertTrue(set(parser.preloads) <= set(urls))
        self.assertLess(sum(urls[url]["bytes"] for url in parser.preloads), 64 * 1024)
        self.assertTrue(all(url.startswith("/phase11/") and urlsplit(url).query.startswith("v=") for url in parser.preloads))
        artwork = next(module for module in report["modules"] if module["path"].endswith("ChatBackdropArtwork.js"))
        self.assertLess(artwork["bytes"], 1024)
        self.assertNotIn(artwork["url"], parser.preloads)

    def test_announcement_css_is_discovered_in_html_before_main_styles(self):
        class Styles(HTMLParser):
            def __init__(self): super().__init__(); self.links = []
            def handle_starttag(self, tag, attrs):
                values = dict(attrs)
                if tag == "link" and values.get("rel") == "stylesheet": self.links.append(values["href"])
        parser = Styles()
        parser.feed((ROOT / "frontend/phase11/index.html").read_text(encoding="utf-8"))
        main = next(url for url in parser.links if url.startswith("/phase11/styles.css?"))
        announcement = [url for url in parser.links if url.startswith("/phase11/shared/AnnouncementPlayer/AnnouncementPlayer.css?")]
        self.assertEqual(len(announcement), 1)
        self.assertLess(parser.links.index(announcement[0]), parser.links.index(main))
        self.assertEqual(announcement[0].split("?", 1)[1], main.split("?", 1)[1])
        css = (ROOT / "frontend/phase11/styles.css").read_text(encoding="utf-8")
        self.assertNotIn("@import", css)
        # The admin page has a separate stylesheet and keeps its own dependency.
        self.assertIn("AnnouncementPlayer/AnnouncementPlayer.css", (ROOT / "frontend/phase11/admin/AdminApp/AdminApp.css").read_text(encoding="utf-8"))

    def test_missing_static_and_dynamic_imports_fail(self):
        for reference in ("import './missing.js';", "import('./missing.js');"):
            with self.subTest(reference=reference), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                entry = self.fixture(root)
                (entry.parent / "app.js").write_text(reference, encoding="utf-8")
                with self.assertRaises(FileNotFoundError): analyze(root, entry)

    def test_external_urls_are_ignored_and_local_escape_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            entry = self.fixture(root)
            for ref in ("https://example.invalid/private.js", "//example.invalid/a.js", "bare-package"):
                self.assertIsNone(local_reference(root, entry, ref))
            with self.assertRaises((ValueError, FileNotFoundError)):
                local_reference(root, entry, "../../../outside.js")


if __name__ == "__main__": unittest.main()
