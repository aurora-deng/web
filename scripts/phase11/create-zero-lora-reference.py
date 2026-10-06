#!/usr/bin/env python3
"""Create a zero-valued PEFT LoRA used as an Adapter comparison baseline.

The output keeps the approved learning Adapter's tensor names and shapes but
replaces every value with zero. Running the zero and non-zero artifacts through
the same Adapter-ready ONNX graph isolates the Adapter effect from graph-export
and quantization differences. It is a test fixture, not a trained Adapter.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path

import torch
from safetensors.torch import load_file, save_file


DEFAULT_SOURCE = Path(
    "/home/pikachu/phase11-models/adapter-learning/"
    "qwen2.5-0.5b-instruct/peft-learning-adapter"
)
DEFAULT_OUTPUT = Path(
    "/home/pikachu/phase11-models/adapter-learning/"
    "qwen2.5-0.5b-instruct/peft-zero-reference-adapter"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--create", action="store_true")
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    if not args.create:
        parser.error("--create is required")

    source = args.source.expanduser().resolve(strict=True)
    output = args.output.expanduser().resolve()
    if source == output:
        raise ValueError("zero-reference output must differ from source")

    source_weights = source / "adapter_model.safetensors"
    source_config = source / "adapter_config.json"
    if not source_weights.is_file() or not source_config.is_file():
        raise FileNotFoundError("source PEFT Adapter is incomplete")

    tensors = load_file(source_weights, device="cpu")
    if not tensors:
        raise ValueError("source PEFT Adapter contains no tensors")
    zero_tensors = {
        name: torch.zeros_like(tensor, memory_format=torch.contiguous_format)
        for name, tensor in tensors.items()
    }

    output.mkdir(parents=True, exist_ok=True)
    output_weights = output / "adapter_model.safetensors"
    save_file(zero_tensors, output_weights)
    shutil.copy2(source_config, output / "adapter_config.json")

    if any(torch.count_nonzero(tensor).item() for tensor in zero_tensors.values()):
        raise RuntimeError("zero-reference Adapter unexpectedly contains non-zero values")

    metadata = {
        "schema": "phase11-zero-adapter-reference-v1",
        "trained": False,
        "purpose": "same-graph zero Adapter comparison fixture",
        "tensor_count": len(zero_tensors),
        "parameter_count": sum(tensor.numel() for tensor in zero_tensors.values()),
        "source_weights_sha256": sha256(source_weights),
        "weights_sha256": sha256(output_weights),
    }
    (output / "PHASE11_ZERO_ADAPTER.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(metadata, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
