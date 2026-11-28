#!/usr/bin/env python3
"""Compatibility entrypoint; implementation: scripts/ai/toolchain/olive_oga017_compat.py."""
from pathlib import Path
import sys
from importlib import import_module
ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path: sys.path.insert(0, str(ROOT))
_implementation = import_module("scripts.ai.toolchain.olive_oga017_compat")
for _name in dir(_implementation):
    if not _name.startswith("__"): globals()[_name] = getattr(_implementation, _name)
if __name__ == "__main__":
    raise SystemExit(_implementation.main())
