"""Migration parity and fail-closed catalog loading; no production data access."""
from dataclasses import asdict
from enum import Enum
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from backend.agent.tool_runtime import catalog
from backend.agent.tool_runtime.registry import default_registry
from scripts.project.generate_skill_catalog import generate, OUTPUT


def plain(value):
    if isinstance(value, Enum): return value.value
    if isinstance(value, dict): return {k: plain(v) for k, v in value.items()}
    if isinstance(value, (tuple, list)): return [plain(v) for v in value]
    return value


class CatalogMigration(unittest.TestCase):
    def test_contracts_and_order_match_pre_migration_snapshot(self):
        before = json.loads((ROOT / "tests/project/fixtures/agent_catalog_v1.json").read_text(encoding="utf-8"))
        registry = default_registry()
        for kind in ("tools", "skills"):
            self.assertEqual([plain(asdict(s)) for s in getattr(registry, kind).values()], before[kind])
        self.assertEqual((len(registry.tools), len(registry.skills)), (18, 19))
        self.assertEqual(OUTPUT.read_text(encoding="utf-8"), generate())
        self.assertEqual(hashlib.sha256(OUTPUT.read_bytes().replace(b"\r\n", b"\n")).hexdigest(), before["nativeCatalogSha256"])

    def test_tool_implementations_cover_both_adapters_and_are_not_mutable_cache(self):
        for adapter in ("workspace", "nebula"):
            reads, writes = catalog.load_tool_handlers(adapter)
            self.assertEqual(len(reads), 15)
            self.assertEqual(set(writes), {"memory.write", "progress.write", "mistake.write"})
            for name, handler in {**reads, **writes}.items():
                self.assertEqual(handler.__module__, "backend.agent.tools." + name + ".impl")
            reads.clear()
            self.assertEqual(len(catalog.load_tool_handlers(adapter)[0]), 15)
        with self.assertRaises(ValueError):
            catalog.load_tool_handlers("../outside")

    def test_cached_schema_is_not_exposed_to_callers(self):
        specs = catalog.load_specs("tools")
        specs[0].parameters["required"].clear()
        self.assertEqual(catalog.load_specs("tools")[0].parameters["required"], ["taskId"])

    def fixture(self, root):
        manifest = {"version": 1, "tools": ["chat.search"], "skills": ["chat.recall"]}
        path = root / "catalog.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        for kind, name in (("tools", "search"), ("skills", "recall")):
            directory = root / kind / "chat" / name
            directory.mkdir(parents=True)
            for file in ("spec.py", "README.md", "impl.py"):
                (directory / file).write_text("checked-in test fixture", encoding="utf-8")
        return path, manifest

    def test_missing_spec_document_or_recipe_is_rejected(self):
        for kind, name, file in (("tools", "search", "spec.py"), ("tools", "search", "impl.py"), ("tools", "search", "README.md"), ("skills", "recall", "impl.py")):
            with self.subTest(kind=kind, file=file), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                path, _ = self.fixture(root)
                (root / kind / "chat" / name / file).unlink()
                with patch.object(catalog, "AGENT_ROOT", root), patch.object(catalog, "CATALOG_PATH", path):
                    with self.assertRaises(ValueError): catalog._entries(kind)

    def test_duplicate_and_path_traversal_names_are_rejected(self):
        for names in (["chat.search", "chat.search"], ["../outside"], ["chat.search/escape"], [True]):
            with self.subTest(names=names), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                path, manifest = self.fixture(root)
                manifest["tools"] = names
                path.write_text(json.dumps(manifest), encoding="utf-8")
                with patch.object(catalog, "AGENT_ROOT", root), patch.object(catalog, "CATALOG_PATH", path):
                    with self.assertRaises(ValueError): catalog._entries("tools")

    def test_unlisted_contract_and_empty_documentation_are_rejected(self):
        for extra in (True, False):
            with self.subTest(extra=extra), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                path, _ = self.fixture(root)
                if extra:
                    directory = root / "tools/chat/unlisted"
                    directory.mkdir()
                    (directory / "spec.py").write_text("unlisted", encoding="utf-8")
                else:
                    (root / "tools/chat/search/README.md").write_text(" ", encoding="utf-8")
                with patch.object(catalog, "AGENT_ROOT", root), patch.object(catalog, "CATALOG_PATH", path):
                    with self.assertRaises(ValueError): catalog._entries("tools")


if __name__ == "__main__": unittest.main()
