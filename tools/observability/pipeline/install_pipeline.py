"""Install a separately approved Linux telemetry package within its disk budget."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile

from tools.observability.pipeline.install_metrics_backend import ROOT, fetch, sha


@dataclass(frozen=True)
class Package:
    directory: str
    version: str
    repo: str
    archive: str
    checksum: str
    programs: tuple
    budget_mib: int
    license: str
    license_checksum: str | None = None


PACKAGES = {
    "collector": Package("otelcol-0.165.0", "0.165.0", "open-telemetry/opentelemetry-collector-releases",
        "otelcol_0.165.0_linux_amd64.tar.gz", "78570dd4bd6570bc41eb327869ef913bbf91b87dd8170ebebbf43bec71f6370d",
        ("otelcol",), 180, "Apache-2.0", "c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4"),
    "tempo": Package("tempo-3.0.3", "3.0.3", "grafana/tempo",
        "tempo_3.0.3_linux_amd64.tar.gz", "7ac4f095dfaee3ba77dbdbb23ebd9f8a021cb6895c3602244ee3bf2787d8ceba",
        ("tempo",), 240, "AGPL-3.0"),
    "alertmanager": Package("alertmanager-0.34.2", "0.34.2", "prometheus/alertmanager",
        "alertmanager-0.34.2.linux-amd64.tar.gz", "17ee14acd0f6ba6e25a436ff223d9c17ddd5cf484f543ad12f0932eaa89ba186",
        ("alertmanager", "amtool"), 200, "Apache-2.0"),
}


class VerifiedStream:
    def __init__(self, source, limit):
        self.source = source
        self.limit = limit
        self.digest = hashlib.sha256()
        self.bytes_read = 0

    def read(self, size):
        data = self.source.read(size)
        self.bytes_read += len(data)
        if self.bytes_read > self.limit:
            raise RuntimeError("Stream exceeded the archive size budget")
        self.digest.update(data)
        return data

    def verify(self, expected):
        while self.read(1024**2):
            pass
        if self.digest.hexdigest() != expected:
            raise RuntimeError("Official asset SHA-256 mismatch")


def extract_programs(package, bundle, payload, remaining, license_file):
    required = {*package.programs, "LICENSE"}
    allowed = required | {"NOTICE"}
    seen = set()
    for member in bundle:
        name = Path(member.name).name
        if name not in allowed:
            continue
        if name in seen or not member.isfile() or not 0 < member.size <= remaining:
            raise RuntimeError(f"Invalid or oversized release member {name}")
        seen.add(name)
        target = payload / name
        with bundle.extractfile(member) as incoming, target.open("xb") as outgoing:
            shutil.copyfileobj(incoming, outgoing, 1024**2)
        target.chmod(0o755 if name in package.programs else 0o644)
        remaining -= member.size
    if "LICENSE" not in seen and license_file is not None:
        if (package.license_checksum is None or license_file.is_symlink() or not license_file.is_file()
                or not 0 < license_file.stat().st_size <= min(32768, remaining)
                or sha(license_file) != package.license_checksum):
            raise RuntimeError("Invalid license from the pinned upstream release")
        target = payload / "LICENSE"
        shutil.copyfile(license_file, target)
        target.chmod(0o644)
        seen.add("LICENSE")
    if required - seen:
        raise RuntimeError(f"Missing release members: {', '.join(sorted(required - seen))}")


def install(package, cache=None, *, stream=None, license_file=None, reserve_mib=2048):
    destination = ROOT / package.directory
    if destination.exists() or destination.is_symlink():
        raise RuntimeError(f"Reuse or inspect the existing directory {destination}")
    budget = package.budget_mib * 1024**2
    cached = cache/package.archive if cache else None
    if cached and (cached.is_symlink() or not cached.is_file() or cached.stat().st_size > budget):
        raise RuntimeError(f"Invalid local release archive {package.archive}")
    cached_size = cached.stat().st_size if cached else 0
    required = reserve_mib * 1024**2 + budget - cached_size
    free = shutil.disk_usage(ROOT).free
    if free < required:
        raise RuntimeError(f"Insufficient space for {package.directory}: {free // 1024**2} MiB free, "
                           f"{(required + 1024**2 - 1) // 1024**2} MiB required")
    url = f"https://github.com/{package.repo}/releases/download/v{package.version}/{package.archive}"
    with tempfile.TemporaryDirectory(prefix="pipeline-", dir=ROOT) as stage:
        directory = Path(stage)
        payload = directory / "installed"
        payload.mkdir()
        if stream is not None:
            verified = VerifiedStream(stream, budget)
            with tarfile.open(fileobj=verified, mode="r|gz") as bundle:
                extract_programs(package, bundle, payload, budget, license_file)
            verified.verify(package.checksum)
            downloaded = verified.bytes_read
        else:
            archive = cached or directory/package.archive
            downloaded = cached_size if cached else fetch(url, archive, budget)
            if sha(archive) != package.checksum:
                raise RuntimeError("Official asset SHA-256 mismatch")
            with tarfile.open(archive) as bundle:
                extract_programs(package, bundle, payload, budget - downloaded, license_file)
        programs = {}
        for name in package.programs:
            output = subprocess.run([str(payload/name), "--version"], check=True, timeout=10, capture_output=True)
            if package.version.encode() not in output.stdout + output.stderr:
                raise RuntimeError(f"Unexpected installed version of {name}")
            programs[name] = sha(payload/name)
        report = {"directory": str(destination), "source": url, "sha256": package.checksum,
                  "downloadedBytes": downloaded, "license": package.license, "programs": programs}
        (payload / "install.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        payload.rename(destination)
    print(json.dumps(report), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--approved", choices=PACKAGES, nargs="+", required=True)
    assets = parser.add_mutually_exclusive_group()
    assets.add_argument("--asset-dir", type=Path, help="Use securely transferred public release archives with the same pinned checksums")
    assets.add_argument("--asset-stdin", action="store_true", help="Stream one official gzip archive, without keeping a disk copy")
    parser.add_argument("--license-file", type=Path, help="Use the pinned official license when the binary archive omits it")
    parser.add_argument("--reserve-mib", type=int, default=2048, help="Disk space to leave free after installing each package")
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() != "x86_64" or os.geteuid() == 0:
        parser.error("Run as the unprivileged Linux x64 application user")
    if ROOT.resolve() != ROOT or not ROOT.is_dir():
        parser.error("Use the existing approved dependency directory")
    if args.asset_stdin and len(args.approved) != 1:
        parser.error("An archive stream installs exactly one approved package")
    if args.reserve_mib < 1024:
        parser.error("Keep at least 1024 MiB free after installation")
    errors = []
    for name in dict.fromkeys(args.approved):
        try:
            install(PACKAGES[name], args.asset_dir,
                    stream=sys.stdin.buffer if args.asset_stdin else None, license_file=args.license_file,
                    reserve_mib=args.reserve_mib)
        except (OSError, RuntimeError, tarfile.TarError, subprocess.SubprocessError) as error:
            errors.append(f"{name}: {error}")
    if errors:
        raise SystemExit("\n".join(errors))


if __name__ == "__main__":
    main()
