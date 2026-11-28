#!/usr/bin/env python3
"""Compare two ONNX Runtime GenAI Adapters on one Adapter-ready graph.

The comparison uses deterministic decoding settings and reads the next-token
logits immediately after the prompt. This avoids mistaking sampling randomness
for an Adapter effect: both candidates run through the exact same ONNX graph,
and only the active Adapter changes.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import onnxruntime_genai as og


DEFAULT_PROMPT = "The color of a clear daytime sky is"


def next_token_logits(
    model: og.Model,
    adapters: og.Adapters,
    adapter_name: str,
    token_ids: list[int],
) -> np.ndarray:
    params = og.GeneratorParams(model)
    params.set_search_options(
        max_length=len(token_ids) + 2,
        do_sample=False,
    )
    generator = og.Generator(model, params)
    generator.set_active_adapter(adapters, adapter_name)
    generator.append_tokens(token_ids)
    return np.array(generator.get_logits(), copy=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path, help="Adapter-ready GenAI model directory")
    parser.add_argument("zero_adapter", type=Path)
    parser.add_argument("learning_adapter", type=Path)
    parser.add_argument("--prompt", default=DEFAULT_PROMPT)
    args = parser.parse_args()

    model_dir = args.model.expanduser().resolve(strict=True)
    zero_path = args.zero_adapter.expanduser().resolve(strict=True)
    learning_path = args.learning_adapter.expanduser().resolve(strict=True)
    for path in (zero_path, learning_path):
        if not path.is_file() or path.suffix != ".onnx_adapter":
            raise ValueError(f"expected an .onnx_adapter file: {path}")

    model = og.Model(str(model_dir))
    tokenizer = og.Tokenizer(model)
    token_ids = list(tokenizer.encode(args.prompt))
    if not token_ids:
        raise ValueError("comparison prompt encoded to zero tokens")

    adapters = og.Adapters(model)
    adapters.load(str(zero_path), "zero-reference")
    adapters.load(str(learning_path), "learning")
    zero_logits = next_token_logits(model, adapters, "zero-reference", token_ids)
    learning_logits = next_token_logits(model, adapters, "learning", token_ids)

    if zero_logits.shape != learning_logits.shape:
        raise RuntimeError("Adapter comparison produced incompatible logit shapes")
    delta = np.abs(learning_logits - zero_logits)
    changed = int(np.count_nonzero(delta))
    result = {
        "prompt_tokens": len(token_ids),
        "logit_shape": list(zero_logits.shape),
        "changed_logits": changed,
        "max_abs_delta": float(delta.max(initial=0.0)),
        "zero_argmax": int(zero_logits.reshape(-1).argmax()),
        "learning_argmax": int(learning_logits.reshape(-1).argmax()),
    }
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if changed == 0:
        raise RuntimeError("non-zero learning Adapter had no observable logit effect")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
