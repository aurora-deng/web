"""Approved local Qwen source contract shared by training and ONNX export.

Standard library only. No downloads, provider calls, model imports or secret files.
"""
from __future__ import annotations
import hashlib
import json
from pathlib import Path, PurePosixPath
import re

EXPECTED_REPOSITORY = "Qwen/Qwen2.5-0.5B-Instruct"
EXPECTED_REVISION = "7ae557604adf67be50417f59c2c2f167def9a775"
EXPECTED_MODEL_SHA256 = "fdf756fa7fcbe7404d5c60e26bff1a0c8b8aa1f72ced49e7dd0210fe288fb7fe"
REQUIRED_FILES = {"config.json", "model.safetensors", "tokenizer.json", "tokenizer_config.json"}
SOURCE_FILES = REQUIRED_FILES | {"generation_config.json", "vocab.json", "merges.txt", "LICENSE", "README.md", ".gitattributes"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_blob_sha1(path: Path) -> str:
    digest = hashlib.sha1()
    digest.update(f"blob {path.stat().st_size}\0".encode("ascii"))
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def unique_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate model manifest field")
        result[key] = value
    return result


def safe_file(root: Path, name: str) -> Path:
    if not isinstance(name, str) or name not in SOURCE_FILES:
        raise ValueError("unexpected model source file")
    relative = PurePosixPath(name)
    if relative.is_absolute() or ".." in relative.parts or "\\" in name or ":" in name:
        raise ValueError("unsafe model source path")
    path = root / name
    if path.is_symlink() or not path.is_file() or not path.resolve(strict=True).is_relative_to(root):
        raise ValueError("model source must contain regular local files")
    return path


def validate_source(source: Path) -> dict:
    root = source.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("model source must be a directory")
    path = root / "DOWNLOAD_MANIFEST.json"
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= 65536:
        raise ValueError("missing, unsafe or oversized model source manifest")
    manifest = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_keys)
    if not isinstance(manifest, dict) or manifest.get("repository") != EXPECTED_REPOSITORY or manifest.get("revision") != EXPECTED_REVISION:
        raise ValueError("source does not match the approved model repository and revision")
    files = manifest.get("files")
    if not isinstance(files, list) or not 1 <= len(files) <= len(SOURCE_FILES):
        raise ValueError("invalid model source inventory")
    total, seen = 0, set()
    for entry in files:
        if not isinstance(entry, dict):
            raise ValueError("invalid model source inventory entry")
        name = entry.get("path")
        file = safe_file(root, name)
        if name in seen:
            raise ValueError("duplicate model source entry")
        seen.add(name)
        size, kind, expected = entry.get("size"), entry.get("digest_kind"), entry.get("digest")
        if type(size) is not int or not 0 <= size <= 2 * 1024**3 or file.stat().st_size != size:
            raise ValueError("model source file size mismatch")
        if kind not in {"sha256", "lfs-sha256", "git-blob-sha1"}:
            raise ValueError("unsupported model source checksum")
        length = 40 if kind == "git-blob-sha1" else 64
        if not isinstance(expected, str) or re.fullmatch(r"[a-f0-9]{" + str(length) + "}", expected) is None:
            raise ValueError("invalid model source checksum")
        actual = git_blob_sha1(file) if kind == "git-blob-sha1" else sha256(file)
        if actual != expected:
            raise ValueError("model source checksum mismatch: " + name)
        if name == "model.safetensors" and (kind not in {"sha256", "lfs-sha256"} or actual != EXPECTED_MODEL_SHA256):
            raise ValueError("weights do not match the approved base model")
        total += size
    if not REQUIRED_FILES <= seen or type(manifest.get("total_bytes")) is not int or manifest["total_bytes"] != total:
        raise ValueError("incomplete model source or total size mismatch")
    return manifest
