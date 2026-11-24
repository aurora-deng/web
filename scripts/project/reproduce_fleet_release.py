"""Rebuild and verify Fleet/Web releases and containers from one clean checkout."""

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

from runtime_libs import checksum


ROOT = Path(__file__).resolve().parents[2]


def call(*command, env=None):
    subprocess.run(command, cwd=ROOT, env=env, check=True)


def python(script, *args, env):
    call(sys.executable, "-B", ROOT / "scripts/project" / script, *args, env=env)


def dependency_lock(args, env):
    programs = {
        name: Path(shutil.which(name, path=env["PATH"])).resolve()
        for name in ("cmake", "ctest", "c++", "docker", "python3")
    }
    programs.update(
        postgres=args.postgres_prefix / "bin/postgres",
        initdb=args.postgres_prefix / "bin/initdb",
        psql=args.postgres_prefix / "bin/psql",
        protoc=args.grpc_prefix / "bin/protoc",
        grpc_cpp_plugin=args.grpc_prefix / "bin/grpc_cpp_plugin",
        curl_config=args.curl_root / "bin/curl-config",
    )
    artifacts = {}
    for name, path in programs.items():
        row = {"sha256": checksum(path)}
        if name != "grpc_cpp_plugin":
            version = subprocess.check_output([path, "--version"], text=True, env=env, timeout=10)
            row["version"] = version.splitlines()[0]
        artifacts[name] = row

    installations = {
        "postgresql": (args.postgres_prefix, "libpq.so"),
        "libsodium": (args.sodium_prefix, "libsodium.so"),
        "curl": (args.curl_root, "libcurl.so"),
        "grpc": (args.grpc_prefix, "libgrpc++.so"),
        "protobuf": (args.grpc_prefix, "libprotobuf.so"),
    }
    for name, (prefix, library) in installations.items():
        candidates = [prefix / folder / library for folder in ("lib", "lib64")]
        path = next((path for path in candidates if path.is_file()), None)
        if path is None and name in ("grpc", "protobuf"):
            path = next((candidate.with_suffix(".a") for candidate in candidates
                         if candidate.with_suffix(".a").is_file()), None)
        if path is None:
            raise RuntimeError(f"Installed dependency missing: {name}")
        artifacts[name] = {"file": path.resolve().name, "sha256": checksum(path)}
    for name in ("ssl", "crypto"):
        path = args.openssl_lib_dir / f"lib{name}.so"
        artifacts[name] = {"file": path.resolve().name, "sha256": checksum(path)}
    packages = {}
    if shutil.which("rpm"):
        for name in ("glibc", "libstdc++", "openssl-libs", "sqlite-libs", "zlib"):
            found = subprocess.run(["rpm", "-q", name], text=True, capture_output=True, timeout=5)
            if found.returncode == 0:
                packages[name] = found.stdout.strip()
    return {"schemaVersion": 1, "platform": platform.machine(),
            "python": platform.python_version(), "hostPackages": packages, "artifacts": artifacts}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    for name in ("postgres-prefix", "grpc-prefix", "sodium-prefix", "curl-root",
                 "gtest-dir", "openssl-include", "openssl-lib-dir"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--expect-lock", type=Path)
    parser.add_argument("--jobs", default=2, type=int, choices=range(1, 9))
    parser.add_argument("--tag", default="v0.4-fleet")
    parser.add_argument("--test-redis-port", type=int)
    parser.add_argument("--bundled-runtime", action="store_true", help="Verify Docker import without host library mounts")
    args = parser.parse_args()
    if not sys.platform.startswith("linux") or os.getuid() == 0:
        parser.error("use an ordinary Linux account with existing dependencies and Docker access")
    git = lambda *argv: subprocess.check_output(["git", *argv], cwd=ROOT, text=True).strip()
    revision = git("rev-parse", "HEAD")
    if git("status", "--porcelain", "--untracked-files=all"):
        parser.error("verify from a clean Git checkout")

    build = args.output.resolve()
    build.parent.mkdir(parents=True, exist_ok=True)
    build.mkdir()
    prefixes = (args.postgres_prefix, args.sodium_prefix, args.curl_root, args.grpc_prefix)
    runtime_paths = [str(prefix / folder) for prefix in prefixes for folder in ("lib", "lib64")
                     if (prefix / folder).is_dir()]
    inherited = [path for path in os.environ.get("LD_LIBRARY_PATH", "").split(os.pathsep) if path]
    env = dict(os.environ, LD_LIBRARY_PATH=os.pathsep.join(runtime_paths + inherited),
               PATH=os.pathsep.join([str(args.grpc_prefix / "bin"), os.environ["PATH"]]))
    lock = dependency_lock(args, env)
    if args.expect_lock and json.loads(args.expect_lock.read_text(encoding="utf-8")) != lock:
        raise RuntimeError("Installed dependency versions or SHA-256 differ from the expected lock")
    (build / "dependency-lock.json").write_text(json.dumps(lock, sort_keys=True, indent=2) + "\n", encoding="utf-8")

    fleet, web = build / "fleet", build / "web"
    python("cpp_systems.py", "--build-dir", fleet, "--with-grpc", "--prefix", args.grpc_prefix,
           "--with-ollama", "--curl-root", args.curl_root, "--openssl-root", args.openssl_include.resolve().parent,
           "--postgres-prefix", args.postgres_prefix, env=env)
    flags = ("--test-redis-port", str(args.test_redis_port)) if args.test_redis_port else ()
    python("inbox_upgrade.py", "--postgres-prefix", args.postgres_prefix, "--output", build / "inbox-upgrade.json", env=env)
    python("reproduce.py", "business", "--build-dir", web, "--jobs", str(args.jobs), "--with-otlp",
           "--postgres-prefix", args.postgres_prefix, "--sodium-prefix", args.sodium_prefix,
           "--curl-root", args.curl_root, "--gtest-dir", args.gtest_dir,
           "--openssl-include", args.openssl_include, "--openssl-lib-dir", args.openssl_lib_dir,
           *flags, env=env)
    with tempfile.TemporaryDirectory(prefix="nebula-v04-archives-", dir=build) as directory:
        zip_dir = Path(directory) / "zip"
        python("package_release.py", "--revision", revision, "package", "--build", web,
               "--fleet-build", fleet, "--format", "zip", "--output", zip_dir, env=env)
        archive = zip_dir / "nebula-linux-x86_64.zip"
        python("package_release.py", "--revision", revision, "verify", archive,
               "--postgres-prefix", args.postgres_prefix, env=env)
        python("offline_docker.py", archive, "--postgres-prefix", args.postgres_prefix, "--output", build / "docker",
               "--tag", args.tag, "--discard-archive", *(("--bundled-runtime",) if args.bundled_runtime else ()), env=env)
    docker = json.loads((build / "docker/delivery-verification.json").read_text(encoding="utf-8"))
    if not (docker.get("loadedArchive") and docker["fleet"].get("checkpointReplay") and
            docker.get("workflow") and docker["workflow"].get("forkJoin")):
        raise RuntimeError("Expected Docker capabilities did not execute")
    if args.bundled_runtime and (docker.get("hostRuntimeMounted") or not docker.get("tagsRemovedBeforeImport")):
        raise RuntimeError("Bundled Docker smoke test was not verified after removing the built tags")
    print(f"Verified clean Git revision {revision}. Reports are under {build}")


if __name__ == "__main__":
    main()
