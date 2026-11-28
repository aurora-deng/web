"""Model comparison CLI contract tests; no real model, network, or database."""
import argparse
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.function3.validation.model_selection import add_model_arguments, resolve_selection, DEFAULT_MODEL


class ModelArguments(unittest.TestCase):
    def parse(self, root, *args):
        parser = argparse.ArgumentParser()
        add_model_arguments(parser)
        return parser.parse_args(["--model-root", str(root), *args])

    def model(self, root, relative):
        directory = root / relative
        directory.mkdir(parents=True)
        (directory / "genai_config.json").write_text("{}", encoding="utf-8")
        return directory

    def test_default_remains_phi3_without_adapter(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.model(root, DEFAULT_MODEL)
            selected = resolve_selection(self.parse(root))
            self.assertEqual(selected.artifact, DEFAULT_MODEL)
            self.assertEqual(selected.label, DEFAULT_MODEL)
            self.assertEqual(selected.adapter, "")

    def test_nested_qwen_model_and_adapter_are_preserved(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            directory = self.model(root, "adapter-learning/qwen/onnx")
            (directory / "trained.onnx_adapter").write_bytes(b"fixture-only")
            args = self.parse(root, "--model-dir", "adapter-learning/qwen/onnx", "--adapter", "trained.onnx_adapter", "--label", "qwen05-smoke")
            selected = resolve_selection(args)
            self.assertEqual(selected.artifact, "adapter-learning/qwen/onnx")
            self.assertEqual(selected.adapter, "trained.onnx_adapter")
            self.assertEqual(resolve_selection(self.parse(root, *selected.arguments())), selected)

    def test_outside_model_and_missing_config_are_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            root = base / "models"
            root.mkdir()
            outside = self.model(base, "outside")
            missing = root / "missing-config"
            missing.mkdir()
            for directory in (outside, missing, root):
                with self.subTest(directory=directory), self.assertRaises(ValueError):
                    resolve_selection(self.parse(root, "--model-dir", str(directory)))

    def test_adapter_escape_wrong_extension_and_bad_label_are_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            directory = self.model(root, DEFAULT_MODEL)
            (root / "outside.onnx_adapter").write_bytes(b"fixture")
            (directory / "wrong.txt").write_bytes(b"fixture")
            for args in (("--adapter", "../outside.onnx_adapter"), ("--adapter", str(root / "outside.onnx_adapter")), ("--adapter", "wrong.txt"), ("--label", "../escape"), ("--label", "")):
                with self.subTest(args=args), self.assertRaises(ValueError):
                    resolve_selection(self.parse(root, *args))


if __name__ == "__main__": unittest.main()
