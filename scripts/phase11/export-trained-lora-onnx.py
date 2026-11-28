#!/usr/bin/env python3
"""Compatibility entrypoint; implementation lives under scripts/ai/."""
from pathlib import Path
import sys
from importlib import import_module
ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path: sys.path.insert(0, str(ROOT))
_implementation = import_module("scripts.ai.export.export_trained_lora_onnx")
# Preserve import-based callers as well as the historical CLI.
for _name in dir(_implementation):
    if not _name.startswith("_"): globals()[_name] = getattr(_implementation, _name)
if __name__ == "__main__":
    raise SystemExit(_implementation.main())
