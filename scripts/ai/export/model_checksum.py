#!/usr/bin/env python3
"""计算 InProcessOnnxModelProvider 使用的 sha256-tree-v1 模型目录摘要。"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


def encode_u64(value: int) -> bytes:
    return value.to_bytes(8, byteorder="big", signed=False)


def tree_checksum(root: Path) -> str:
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("model path is not a directory")

    files: list[tuple[str, Path]] = []
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"symbolic link is not allowed: {path}")
        if path.is_dir():
            continue
        if not path.is_file():
            raise ValueError(f"special file is not allowed: {path}")
        files.append((path.relative_to(root).as_posix(), path))

    digest = hashlib.sha256()
    for relative, path in sorted(files):
        encoded_path = relative.encode("utf-8")
        digest.update(encode_u64(len(encoded_path)))
        digest.update(encoded_path)
        digest.update(encode_u64(path.stat().st_size))
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(64 * 1024), b""):
                digest.update(chunk)
    return "sha256-tree-v1:" + digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Compute the Phase 11 ONNX model directory checksum"
    )
    parser.add_argument("model_dir", type=Path)
    args = parser.parse_args()
    print(tree_checksum(args.model_dir))


if __name__ == "__main__":
    main()
