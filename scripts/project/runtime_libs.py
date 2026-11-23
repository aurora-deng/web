"""Stage the Linux loader and shared libraries from the current machine for offline containers."""
import argparse
from hashlib import sha256
import json
from pathlib import Path
import platform
import re
import subprocess
import sys


PROGRAMS = (
    "webserver",
    "fleet/nebula_fleet_coordinator",
    "fleet/nebula_fleet_edge",
    "fleet/nebula_fleet_ctl",
    "fleet/nebula_fleet_agent",
)
LOADER = "ld-linux-x86-64.so.2"


def checksum(path):
    with path.open("rb") as data:
        digest = sha256()
        for chunk in iter(lambda: data.read(1024 * 1024), b""):
            digest.update(chunk)
        return digest.hexdigest()


def libraries(binary):
    output = subprocess.run(["ldd", str(binary)], check=True, capture_output=True, text=True, timeout=10).stdout
    resolved = {}
    for entry in output.splitlines():
        line = entry.strip()
        if not line or line.startswith("linux-vdso."):
            continue
        if line.endswith("=> not found"):
            raise ValueError(f"{binary.name} needs an existing compatible {line.split()[0]}")
        match = re.fullmatch(r"(?:(\S+) => )?(/[^\n]+?) \(0x[0-9a-f]+\)", line)
        if not match:
            raise ValueError(f"Unrecognized loader result for {binary.name}")
        source = Path(match[2]).resolve(strict=True)
        name = match[1] or Path(match[2]).name
        if not re.fullmatch(r"[A-Za-z0-9._+-]+\.so(?:\.[0-9]+)*", name) or not source.is_file():
            raise ValueError(f"Invalid shared library for {binary.name}")
        resolved[name] = source
    if LOADER not in resolved:
        raise ValueError(f"Expected the Linux x86-64 loader for {binary.name}")
    return resolved


def prepare(root, output, role="web"):
    if not sys.platform.startswith("linux") or platform.machine() != "x86_64":
        raise ValueError("Use a Linux x86-64 host with the compatible local runtime libraries")
    manifest = json.loads((root / "release.json").read_text(encoding="utf-8"))
    sources = {}
    hashes = {}
    programs = PROGRAMS[:1] if role == "web" else PROGRAMS[1:]
    for program in programs:
        binary = root / program
        if checksum(binary) != manifest["files"].get(program):
            raise ValueError(f"Release checksum mismatch for {program}")
        for name, source in libraries(binary).items():
            digest = checksum(source)
            if name in hashes and hashes[name] != digest:
                raise ValueError(f"{role} programs resolve conflicting versions of {name}")
            hashes[name] = digest
            sources[name] = source
    output.mkdir(mode=0o755, parents=True, exist_ok=False)
    output.chmod(0o755)
    for name, source in sorted(sources.items()):
        target = output / name
        with target.open("xb") as file:
            file.write(source.read_bytes())
        if checksum(target) != hashes[name]:
            raise ValueError(f"Library changed during preparation: {name}")
        target.chmod(0o555 if name == LOADER else 0o444)
    report = {"architecture": "linux/amd64", "revision": manifest["revision"], "role": role, "libraries": hashes}
    (output / "libraries.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="Extracted, verified NEBULA Linux release")
    parser.add_argument("--role", choices=("web", "fleet"), default="web")
    parser.add_argument("--output", type=Path, required=True, help="New mount directory for local libraries")
    args = parser.parse_args()
    try:
        report = prepare(args.root.resolve(), args.output.resolve(), args.role)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Cannot prepare the compatible local runtime: {error}\n")
    print(f"Prepared {len(report['libraries'])} compatible libraries for {report['revision']}")


if __name__ == "__main__":
    main()
