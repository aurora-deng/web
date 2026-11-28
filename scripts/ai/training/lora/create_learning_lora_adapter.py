#!/usr/bin/env python3
"""Create a deterministic, untrained PEFT LoRA for Adapter pipeline tests.

This artifact is intentionally not presented as a fine-tuned model. Its only
job is to put non-zero LoRA tensors through Olive, ONNX Runtime GenAI, and the
Phase 11 C++ lifecycle so the integration can be verified without pretending
that CPU-side random weights learned useful behavior.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import torch
from peft import LoraConfig, TaskType, get_peft_model
from transformers import AutoModelForCausalLM


EXPECTED_REPOSITORY = "Qwen/Qwen2.5-0.5B-Instruct"
EXPECTED_REVISION = "7ae557604adf67be50417f59c2c2f167def9a775"
DEFAULT_SOURCE = Path(
    "/home/pikachu/phase11-models/adapter-learning/"
    "qwen2.5-0.5b-instruct/source"
)
DEFAULT_OUTPUT = Path(
    "/home/pikachu/phase11-models/adapter-learning/"
    "qwen2.5-0.5b-instruct/peft-learning-adapter"
)
SEED = 1104


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_source(source: Path) -> None:
    manifest_path = source / "DOWNLOAD_MANIFEST.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("repository") != EXPECTED_REPOSITORY:
        raise ValueError("unexpected base-model repository")
    if manifest.get("revision") != EXPECTED_REVISION:
        raise ValueError("unexpected base-model revision")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--create",
        action="store_true",
        help="confirm creation of the approved local learning artifact",
    )
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    if not args.create:
        parser.error("--create is required")

    source = args.source.expanduser().resolve(strict=True)
    output = args.output.expanduser().resolve()
    validate_source(source)
    output.mkdir(parents=True, exist_ok=True)

    torch.manual_seed(SEED)
    model = AutoModelForCausalLM.from_pretrained(
        source,
        local_files_only=True,
        dtype=torch.float32,
        low_cpu_mem_usage=True,
    )
    config = LoraConfig(
        task_type=TaskType.CAUSAL_LM,
        inference_mode=True,
        r=4,
        lora_alpha=8,
        lora_dropout=0.0,
        bias="none",
        target_modules=["q_proj", "k_proj", "v_proj", "o_proj"],
    )
    model = get_peft_model(model, config)

    adapter_parameter_count = 0
    with torch.no_grad():
        for name, parameter in model.named_parameters():
            if "lora_A" not in name and "lora_B" not in name:
                continue
            # Both matrices must be non-zero. PEFT normally starts LoRA B at
            # zero, which would make Base and Adapter outputs identical.
            parameter.normal_(mean=0.0, std=0.01)
            adapter_parameter_count += parameter.numel()

    if adapter_parameter_count == 0:
        raise RuntimeError("PEFT did not create any LoRA parameters")

    model.save_pretrained(output, safe_serialization=True)
    weights = output / "adapter_model.safetensors"
    if not weights.is_file():
        raise RuntimeError("PEFT did not save adapter_model.safetensors")

    metadata = {
        "schema": "phase11-learning-adapter-v1",
        "base_repository": EXPECTED_REPOSITORY,
        "base_revision": EXPECTED_REVISION,
        "seed": SEED,
        "trained": False,
        "purpose": "Adapter export and runtime lifecycle verification only",
        "adapter_parameter_count": adapter_parameter_count,
        "weights_sha256": sha256(weights),
    }
    (output / "PHASE11_LEARNING_ADAPTER.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(metadata, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
