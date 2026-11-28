#!/usr/bin/env python3
"""Download the approved Qwen Adapter learning model at an immutable revision.

This script deliberately keeps the complete file manifest in source control.
The model host may redirect large files to object storage, but every downloaded
file is accepted only after its size and Git/LFS digest match this manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import quote

import requests


REPOSITORY = "Qwen/Qwen2.5-0.5B-Instruct"
REVISION = "7ae557604adf67be50417f59c2c2f167def9a775"
DEFAULT_OUTPUT = Path(
    "/home/pikachu/phase11-models/adapter-learning/"
    "qwen2.5-0.5b-instruct/source"
)


@dataclass(frozen=True)
class ModelFile:
    path: str
    size: int
    digest_kind: str
    digest: str


FILES = (
    ModelFile(".gitattributes", 1519, "git-blob-sha1", "a6344aac8c09253b3b630fb776ae94478aa0275b"),
    ModelFile("LICENSE", 11343, "git-blob-sha1", "6634c8cc3133b3848ec74b9f275acaaa1ea618ab"),
    ModelFile("README.md", 4917, "git-blob-sha1", "4b8373851d093eb9f3017443f27781c6971eff24"),
    ModelFile("config.json", 659, "git-blob-sha1", "0dbb161213629a23f0fc00ef286e6b1e366d180f"),
    ModelFile("generation_config.json", 242, "git-blob-sha1", "dfc11073787daf1b0f9c0f1499487ab5f4c93738"),
    ModelFile("merges.txt", 1671839, "git-blob-sha1", "20024bfe7c83998e9aeaf98a0cd6a2ce6306c2f0"),
    ModelFile("model.safetensors", 988097824, "sha256", "fdf756fa7fcbe7404d5c60e26bff1a0c8b8aa1f72ced49e7dd0210fe288fb7fe"),
    ModelFile("tokenizer.json", 7031645, "git-blob-sha1", "443909a61d429dff23010e5bddd28ff530edda00"),
    ModelFile("tokenizer_config.json", 7305, "git-blob-sha1", "07bfe0640cb5a0037f9322287fbfc682806cf672"),
    ModelFile("vocab.json", 2776833, "git-blob-sha1", "4783fe10ac3adce15ac8f358ef5462739852c569"),
)


def file_digest(path: Path, digest_kind: str) -> str:
    if digest_kind == "sha256":
        digest = hashlib.sha256()
        prefix = b""
    elif digest_kind == "git-blob-sha1":
        digest = hashlib.sha1()
        prefix = f"blob {path.stat().st_size}\0".encode("ascii")
    else:
        raise ValueError(f"unsupported digest kind: {digest_kind}")

    digest.update(prefix)
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify(path: Path, spec: ModelFile) -> None:
    if path.stat().st_size != spec.size:
        raise ValueError(
            f"size mismatch for {spec.path}: {path.stat().st_size} != {spec.size}"
        )
    actual = file_digest(path, spec.digest_kind)
    if actual != spec.digest:
        raise ValueError(
            f"{spec.digest_kind} mismatch for {spec.path}: {actual} != {spec.digest}"
        )


def download(session: requests.Session, url: str, target: Path, spec: ModelFile) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(target.name + ".part")
    if partial.exists():
        try:
            verify(partial, spec)
            os.replace(partial, target)
            return
        except ValueError:
            pass
    offset = partial.stat().st_size if partial.exists() else 0
    headers = {"Range": f"bytes={offset}-"} if offset else {}

    with session.get(url, headers=headers, stream=True, timeout=(30, 120)) as response:
        response.raise_for_status()
        # A server may ignore Range and return 200. Restart instead of appending
        # a second full copy to the partial file.
        append = offset > 0 and response.status_code == 206
        mode = "ab" if append else "wb"
        with partial.open(mode) as sink:
            for chunk in response.iter_content(chunk_size=1024 * 1024):
                if chunk:
                    sink.write(chunk)

    verify(partial, spec)
    os.replace(partial, target)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--install",
        action="store_true",
        help="confirm that the approved model files may be downloaded",
    )
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--base-url",
        default="https://huggingface.co",
        help="transport host; repository, revision and hashes remain fixed",
    )
    args = parser.parse_args()
    if not args.install:
        parser.error("--install is required to perform the approved download")

    output = args.output.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    session = requests.Session()
    session.headers["User-Agent"] = "phase11-qwen-adapter-learning/1.0"

    manifest_files = []
    for spec in FILES:
        target = output / spec.path
        encoded_path = quote(spec.path, safe="/")
        url = (
            f"{args.base_url.rstrip('/')}/{REPOSITORY}/resolve/"
            f"{REVISION}/{encoded_path}"
        )
        if target.exists():
            try:
                verify(target, spec)
                print(f"verified existing: {spec.path}", flush=True)
            except ValueError:
                # Preserve the invalid file for diagnosis; never silently trust
                # or overwrite bytes that failed the immutable manifest.
                rejected = target.with_name(target.name + ".rejected")
                os.replace(target, rejected)
                print(f"quarantined invalid file: {rejected}", flush=True)
                download(session, url, target, spec)
        else:
            print(f"downloading: {spec.path}", flush=True)
            download(session, url, target, spec)

        verify(target, spec)
        manifest_files.append(
            {
                "path": spec.path,
                "size": spec.size,
                "digest_kind": spec.digest_kind,
                "digest": spec.digest,
                "transport_url": url,
            }
        )

    manifest = {
        "schema": "phase11-model-download-v1",
        "repository": REPOSITORY,
        "revision": REVISION,
        "license": "Apache-2.0",
        "total_bytes": sum(item.size for item in FILES),
        "files": manifest_files,
    }
    manifest_path = output / "DOWNLOAD_MANIFEST.json"
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"model ready: {output}")
    print(f"manifest: {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
