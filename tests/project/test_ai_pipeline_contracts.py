"""Synthetic model files and mocked Olive; no model imports, downloads or real training."""
import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.ai.contracts import qwen_source as source
from scripts.ai.export import export_trained_lora_onnx as export


class PipelineContracts(unittest.TestCase):
    def setUp(self):
        base = ROOT / "build-function3/ai-pipeline-contracts"
        base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.model = self.root / "source"
        self.model.mkdir()
        self.files = {"config.json": b"{}", "tokenizer.json": b"{}", "tokenizer_config.json": b"{}",
                      "model.safetensors": b"not-a-real-model-fixture"}
        for name, data in self.files.items(): (self.model / name).write_bytes(data)
        self.manifest = {"repository": source.EXPECTED_REPOSITORY, "revision": source.EXPECTED_REVISION,
                         "files": [{"path": name, "size": len(data), "digest_kind": "sha256", "digest": source.sha256(self.model/name)}
                                   for name, data in self.files.items()], "total_bytes": sum(map(len, self.files.values()))}
        self.write_manifest()
        checksum = source.sha256(self.model / "model.safetensors")
        p = patch.object(source, "EXPECTED_MODEL_SHA256", checksum)
        p.start(); self.addCleanup(p.stop)
        self.adapter = self.root / "trained"
        self.adapter.mkdir()
        (self.adapter / "adapter_model.safetensors").write_bytes(b"fixture-trained-weights")
        self.training = {"schema": "phase22-real-lora-training-v1", "trained": True, "rank": 4,
                         "weights_sha256": source.sha256(self.adapter / "adapter_model.safetensors"),
                         "authorization_mode": "pipeline-smoke", "dataset_id": None, "dataset_sha256": "a"*64,
                         "dataset_records": 3, "base_repository": source.EXPECTED_REPOSITORY,
                         "base_revision": source.EXPECTED_REVISION, "base_weights_sha256": checksum}
        self.write_training()

    def write_manifest(self):
        (self.model / "DOWNLOAD_MANIFEST.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def write_training(self):
        (self.adapter / "PHASE22_TRAINING_MANIFEST.json").write_text(json.dumps(self.training), encoding="utf-8")

    def arguments(self, *extra):
        return ["export", "--export", "--source", str(self.model), "--adapter", str(self.adapter),
                "--work", str(self.root / "work"), "--output", str(self.root / "output"), *extra]

    def fake_olive(self, launcher, config):
        self.assertEqual(launcher, ROOT / "scripts/ai/toolchain/olive_oga017_compat.py")
        settings = json.loads(config.read_text())
        output = Path(settings["output_dir"])
        output.mkdir()
        (output / "model.onnx").write_bytes(b"fixture-only-graph")
        if "extract" in settings["passes"]:
            (output / "adapter_weights.onnx_adapter").write_bytes(b"fixture-only-adapter")

    def test_approved_source_inventory_is_verified(self):
        self.assertEqual(source.validate_source(self.model), self.manifest)
        (self.model / "tokenizer.json").write_bytes(b"changed")
        with self.assertRaises(ValueError): source.validate_source(self.model)

    def test_duplicate_missing_or_unsafe_source_entries_rejected(self):
        original = copy.deepcopy(self.manifest)
        for kind in ("duplicate", "missing", "escape", "unknown", "wrongRevision", "wrongWeight", "badKind", "badTotal"):
            self.manifest = copy.deepcopy(original)
            if kind == "duplicate": self.manifest["files"].append(self.manifest["files"][0])
            elif kind == "missing": self.manifest["files"].pop(0)
            elif kind == "escape": self.manifest["files"][0]["path"] = "../outside.json"
            elif kind == "unknown": self.manifest["files"][0]["path"] = "private-data.json"
            elif kind == "wrongRevision": self.manifest["revision"] = "unapproved"
            elif kind == "wrongWeight": self.manifest["files"][-1]["digest"] = "0"*64
            elif kind == "badKind": self.manifest["files"][0]["digest_kind"] = "skip"
            else: self.manifest["total_bytes"] += 1
            self.write_manifest()
            with self.subTest(kind=kind), self.assertRaises(ValueError): source.validate_source(self.model)

    def test_duplicate_manifest_fields_are_rejected(self):
        (self.model/"DOWNLOAD_MANIFEST.json").write_text('{"repository":"a","repository":"b"}', encoding="utf-8")
        with self.assertRaises(ValueError): source.validate_source(self.model)

    def test_export_checks_source_and_lineage_before_running_olive(self):
        for field, value in (("base_revision", "different"), ("base_weights_sha256", "0"*64),
                             ("authorization_mode", "unknown"), ("dataset_id", 7), ("rank", True)):
            original = copy.deepcopy(self.training)
            self.training[field] = value; self.write_training()
            with self.subTest(field=field), patch.object(sys, "argv", self.arguments()), patch.object(export, "run_olive") as run:
                with self.assertRaises(ValueError): export.main()
                run.assert_not_called()
                self.assertFalse((self.root/"work").exists())
            self.training = original
        self.write_training()

    def test_mocked_export_preserves_flat_lineage_and_old_entrypoint(self):
        with patch.object(sys, "argv", self.arguments()), patch.object(export, "run_olive", side_effect=self.fake_olive), patch("builtins.print"):
            self.assertEqual(export.main(), 0)
        lineage = json.loads((self.root/"output/PHASE24_MODEL_LINEAGE.json").read_text())
        self.assertEqual(lineage["schema"], "phase24-model-training-lineage-v1")
        self.assertTrue(all(isinstance(value, str) for value in lineage.values()))
        self.assertEqual(lineage["authorization_mode"], "pipeline-smoke")
        self.assertEqual(lineage["dataset_id"], "")
        spec = importlib.util.spec_from_file_location("export_compat_contract", ROOT/"scripts/phase11/export-trained-lora-onnx.py")
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        self.assertIs(module.build_lineage_manifest, export.build_lineage_manifest)
        self.assertIs(module.main, export.main)

    def test_reference_reuses_only_allowlisted_model_support_files(self):
        reference = self.root/"reference"; reference.mkdir()
        (reference/"model.onnx").write_bytes(b"fixture-only-graph")
        (reference/"genai_config.json").write_text("{}")
        (reference/"tokenizer.json").write_text("{}")
        (reference/"chat_template.jinja").write_text("{{ messages[0]['content'] }}", encoding="utf-8")
        (reference/"extra.private.txt").write_text("synthetic-excluded-file")
        with patch.object(sys,"argv",self.arguments("--reference-deploy",str(reference))), patch.object(export,"run_olive",side_effect=self.fake_olive), patch("builtins.print"):
            self.assertEqual(export.main(),0)
        output=self.root/"output"
        self.assertTrue((output/"genai_config.json").is_file())
        self.assertFalse((output/"extra.private.txt").exists())
        self.assertEqual((output/"chat_template.jinja").read_bytes(), (reference/"chat_template.jinja").read_bytes())
        manifest=json.loads((output/"PHASE22_ONNX_ADAPTER_MANIFEST.json").read_text())
        self.assertEqual(manifest["reference_support_files"],["chat_template.jinja","genai_config.json","tokenizer.json"])

    def test_olive_process_is_offline_and_telemetry_disabled(self):
        with patch.dict(export.os.environ, {"HF_TOKEN": "synthetic-token-not-real", "HTTP_PROXY": "synthetic-proxy",
                                           "PYTHONPATH": "synthetic-injection", "PATH": "fixture-path"}, clear=True), patch.object(export.subprocess,"run") as run:
            export.run_olive(Path("launcher"),Path("config"))
        command=run.call_args.args[0]; environment=run.call_args.kwargs["env"]
        self.assertIn("--disable_telemetry",command)
        self.assertEqual(environment["HF_HUB_OFFLINE"],"1")
        self.assertEqual(environment["TRANSFORMERS_OFFLINE"],"1")
        self.assertEqual(environment["DO_NOT_TRACK"],"1")
        self.assertEqual(environment["HF_HUB_DISABLE_IMPLICIT_TOKEN"],"1")
        self.assertEqual(environment["PATH"],"fixture-path")
        for forbidden in ("HF_TOKEN", "HTTP_PROXY", "PYTHONPATH"):
            self.assertNotIn(forbidden, environment)


if __name__ == "__main__": unittest.main()
