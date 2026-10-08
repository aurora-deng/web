#!/usr/bin/env python3
"""Export a trained Phase 22 PEFT LoRA into an OGA ``.onnx_adapter``.

The existing ONNX graph uses OGA's normalized ``attn`` input names.  A generic
PEFT conversion keeps Transformers' ``self_attn`` names and cannot be loaded by
that graph.  This script therefore runs the same Olive ModelBuilder +
ExtractAdapters pipeline that created the deployable base graph.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def build_lineage_manifest(
    training_manifest: dict,
    training_manifest_sha256: str,
    training_config_sha256: str,
    onnx_adapter_sha256: str,
    onnx_model_sha256: str,
) -> dict[str, str]:
    """Build the flat, language-neutral manifest consumed by the C++ CLI.

    FlatJson intentionally supports only string values.  Keeping this function
    separate gives the exporter and the registration boundary one explicit
    schema instead of relying on two similar-looking nested manifests.
    """
    dataset_id = training_manifest.get("dataset_id")
    return {
        "schema": "phase24-model-training-lineage-v1",
        "authorization_mode": str(training_manifest["authorization_mode"]),
        "dataset_id": "" if dataset_id is None else str(dataset_id),
        "dataset_sha256": str(training_manifest["dataset_sha256"]),
        "dataset_records": str(training_manifest["dataset_records"]),
        "base_repository": str(training_manifest["base_repository"]),
        "base_revision": str(training_manifest["base_revision"]),
        "base_weights_sha256": str(training_manifest["base_weights_sha256"]),
        "training_manifest_sha256": training_manifest_sha256,
        "training_config_sha256": training_config_sha256,
        "peft_weights_sha256": str(training_manifest["weights_sha256"]),
        "onnx_adapter_sha256": onnx_adapter_sha256,
        "onnx_model_sha256": onnx_model_sha256,
    }


def local_system() -> dict:
    return {
        "type": "LocalSystem",
        "accelerators": [
            {"execution_providers": ["CPUExecutionProvider"], "device": "cpu"}
        ],
    }


def run_olive(launcher: Path, config: Path) -> None:
    subprocess.run(
        [
            sys.executable,
            str(launcher),
            "run",
            "--config",
            str(config),
            "--disable_telemetry",
        ],
        check=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export", action="store_true", help="confirm the export")
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--adapter", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reference-deploy", type=Path)
    parser.add_argument("--cleanup-work", action="store_true")
    args = parser.parse_args()
    if not args.export:
        parser.error("--export is required")

    source = args.source.expanduser().resolve(strict=True)
    adapter = args.adapter.expanduser().resolve(strict=True)
    work = args.work.expanduser().resolve()
    output = args.output.expanduser().resolve()
    launcher = Path(__file__).with_name("olive-oga017-compat.py").resolve(strict=True)
    if work.exists() or output.exists():
        raise FileExistsError("work and output paths must not already exist")

    training_manifest_path = adapter / "PHASE22_TRAINING_MANIFEST.json"
    training_manifest = json.loads(training_manifest_path.read_text(encoding="utf-8"))
    if training_manifest.get("schema") != "phase22-real-lora-training-v1":
        raise ValueError("adapter is missing a Phase 22 training manifest")
    if training_manifest.get("trained") is not True:
        raise ValueError("adapter manifest does not describe trained weights")
    if training_manifest.get("rank") != 4:
        raise ValueError("the current ONNX Adapter graph requires LoRA rank 4")
    weights = adapter / "adapter_model.safetensors"
    if sha256(weights) != training_manifest.get("weights_sha256"):
        raise ValueError("trained PEFT weights do not match their manifest")
    training_manifest_sha256 = sha256(training_manifest_path)
    training_config = {
        key: training_manifest.get(key)
        for key in (
            "seed", "max_steps", "max_length", "learning_rate", "rank",
            "alpha", "target_modules", "torch_version",
        )
    }
    training_config_sha256 = sha256_text(json.dumps(
        training_config, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ))

    work.parent.mkdir(parents=True, exist_ok=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    work.mkdir()
    builder_output = work / "model-with-lora"
    builder_config = work / "model-builder.json"
    builder_config.write_text(
        json.dumps(
            {
                "input_model": {
                    "type": "HfModel",
                    "model_path": str(source),
                    "load_kwargs": {
                        "attn_implementation": "eager",
                        "trust_remote_code": False,
                    },
                    "adapter_path": str(adapter),
                },
                "passes": {
                    "model_builder": {
                        "type": "ModelBuilder",
                        "precision": "int4",
                        "int4_block_size": 32,
                        "int4_accuracy_level": 4,
                        "int4_op_types_to_quantize": ["MatMul", "Gather"],
                        "extra_options": {},
                    }
                },
                "systems": {"local_system": local_system()},
                "no_artifacts": True,
                "data_configs": [],
                "target": "local_system",
                "output_dir": str(builder_output),
                "log_severity_level": 1,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    run_olive(launcher, builder_config)

    extract_config = work / "extract-adapter.json"
    extract_config.write_text(
        json.dumps(
            {
                "input_model": {
                    "type": "OnnxModel",
                    "model_path": str(builder_output),
                    "onnx_file_name": "model.onnx",
                },
                "passes": {
                    "extract": {
                        "type": "ExtractAdapters",
                        "save_format": "onnx_adapter",
                        "adapter_type": "lora",
                    }
                },
                "systems": {"local_system": local_system()},
                "host": "local_system",
                "target": "local_system",
                "no_artifacts": True,
                "output_dir": str(output),
                "log_severity_level": 1,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    run_olive(launcher, extract_config)

    exported_adapter = output / "adapter_weights.onnx_adapter"
    exported_model = output / "model.onnx"
    if not exported_adapter.is_file() or not exported_model.is_file():
        raise RuntimeError("Olive did not create the deployable model and adapter")

    reference_sha256 = None
    compacted = False
    support_files: list[str] = []
    if args.reference_deploy:
        reference = args.reference_deploy.expanduser().resolve(strict=True)
        reference_model = reference / "model.onnx"
        reference_sha256 = sha256(reference_model)
        if sha256(exported_model) != reference_sha256:
            raise RuntimeError("trained export changed the base ONNX graph")
        exported_model.unlink()
        os.link(reference_model, exported_model)
        compacted = True
        # ExtractAdapters intentionally emits only the graph, adapter and model
        # metadata.  A loadable OGA directory also needs tokenizer/generation
        # files.  Reuse byte-identical files from the validated base deploy.
        for source_file in reference.iterdir():
            if not source_file.is_file() or source_file.name in {
                "model.onnx",
                "adapter_weights.onnx_adapter",
            }:
                continue
            destination = output / source_file.name
            if destination.exists():
                # ModelBuilder may emit a model_config that records this exact
                # adapter-enabled graph.  Keep Olive's file; the reference only
                # fills files ExtractAdapters omitted (mainly tokenizer data).
                pass
            else:
                os.link(source_file, destination)
            support_files.append(source_file.name)

    manifest = {
        "schema": "phase22-onnx-adapter-export-v1",
        "training_manifest": str(training_manifest_path),
        "training_manifest_sha256": training_manifest_sha256,
        "training_config_sha256": training_config_sha256,
        "authorization_mode": training_manifest["authorization_mode"],
        "dataset_id": training_manifest.get("dataset_id"),
        "dataset_records": training_manifest["dataset_records"],
        "peft_weights_sha256": training_manifest["weights_sha256"],
        "dataset_sha256": training_manifest["dataset_sha256"],
        "onnx_adapter_sha256": sha256(exported_adapter),
        "onnx_model_sha256": sha256(exported_model),
        "reference_model_sha256": reference_sha256,
        "model_hardlinked_to_reference": compacted,
        "reference_support_files": sorted(support_files),
    }
    (output / "PHASE22_ONNX_ADAPTER_MANIFEST.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    # 把训练出生证明带入部署目录，注册 Candidate 时无需访问训练工作目录。
    shutil.copy2(training_manifest_path,
                 output / "PHASE22_TRAINING_MANIFEST.json")
    lineage = build_lineage_manifest(
        training_manifest,
        training_manifest_sha256,
        training_config_sha256,
        sha256(exported_adapter),
        sha256(exported_model),
    )
    (output / "PHASE24_MODEL_LINEAGE.json").write_text(
        json.dumps(lineage, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    if args.cleanup_work:
        shutil.rmtree(work)
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
