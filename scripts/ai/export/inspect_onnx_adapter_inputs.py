#!/usr/bin/env python3
"""Inspect an ONNX graph without loading its external tensor data.

The deployment model keeps its weights in a multi-gigabyte .onnx.data file.
Adapter capability is described by graph inputs and names, so this audit can
remain fast and read-only by parsing only the small graph protobuf.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import onnx


def main() -> int:
    parser = argparse.ArgumentParser(
        description="List ONNX inputs and names that mention LoRA or Adapter."
    )
    parser.add_argument("model", type=Path, help="Path to the .onnx graph")
    parser.add_argument(
        "--summary",
        action="store_true",
        help="print counts without listing hundreds of graph names",
    )
    args = parser.parse_args()

    if not args.model.is_file():
        parser.error(f"model does not exist: {args.model}")

    # External weights are irrelevant to this structural check and can be
    # several gigabytes, so deliberately leave them on disk.
    model = onnx.load(args.model, load_external_data=False)
    graph_inputs = [value.name for value in model.graph.input]
    unique_graph_inputs = set(graph_inputs)
    adapter_inputs = [
        name
        for name in graph_inputs
        if "lora" in name.lower() or "adapter" in name.lower()
    ]
    unique_adapter_inputs = set(adapter_inputs)

    all_names = set(graph_inputs)
    all_names.update(initializer.name for initializer in model.graph.initializer)
    for node in model.graph.node:
        all_names.add(node.name)
        all_names.update(node.input)
        all_names.update(node.output)

    adapter_markers = sorted(
        name
        for name in all_names
        if "lora" in name.lower() or "adapter" in name.lower()
    )

    print(f"graph_inputs={len(graph_inputs)}")
    print(f"unique_graph_inputs={len(unique_graph_inputs)}")
    print(f"duplicate_graph_input_entries={len(graph_inputs) - len(unique_graph_inputs)}")
    print(f"adapter_input_entries={len(adapter_inputs)}")
    print(f"unique_adapter_inputs={len(unique_adapter_inputs)}")
    print(f"adapter_markers={len(adapter_markers)}")
    if not args.summary:
        for name in graph_inputs:
            print(f"input={name}")
        for name in adapter_markers:
            print(f"marker={name}")

    # Exit 3 gives automation a distinct signal: parsing worked, but this graph
    # does not expose an obvious Adapter interface.
    return 0 if adapter_markers else 3


if __name__ == "__main__":
    raise SystemExit(main())
