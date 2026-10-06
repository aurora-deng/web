#!/usr/bin/env python3
"""Report the Adapter toolchain package contract without changing it."""

from __future__ import annotations

import importlib.metadata as metadata
from pathlib import Path

import onnxruntime_genai


def main() -> None:
    oga = metadata.distribution("onnxruntime-genai")
    olive = metadata.distribution("olive-ai")
    package_root = Path(onnxruntime_genai.__file__).parent

    print(f"oga_version={oga.version}")
    print(f"oga_extras={oga.metadata.get_all('Provides-Extra')}")
    print(f"oga_requires={oga.requires}")
    print(f"oga_package_root={package_root}")
    print("oga_model_related_files=")
    for path in sorted(package_root.rglob("*")):
        if "model" in path.name.lower():
            print(path.relative_to(package_root))

    olive_oga_requirements = [
        requirement
        for requirement in olive.requires or []
        if "onnxruntime-genai" in requirement.lower()
    ]
    print(f"olive_version={olive.version}")
    print(f"olive_oga_requirements={olive_oga_requirements}")


if __name__ == "__main__":
    main()
