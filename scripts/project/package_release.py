"""Package and verify the exact runtime that passed the Linux business gate."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import platform
import re
import secrets
import stat
import subprocess
import sys
import tarfile
import tempfile
import zipfile

from build_stamp import fingerprint
from cpp_systems import source_digest as fleet_source_digest
from repro_runtime import clean_environment, disposable_database, http_smoke, owned_server, require

ROOT = Path(__file__).resolve().parents[2]

# Only application runtime files are delivered. Local keys, build logs, test
# databases and model files are outside the allowlisted paths and formats.
RUNTIME_FILES = {
    "frontend/phase11": {".html", ".css", ".js", ".json", ".svg", ".webp", ".png", ".jpg", ".jpeg", ".woff2", ".txt"},
    "frontend/observability": {".html", ".css", ".js", ".svg"},
    "static": {".svg"},
    "tools/observability/observer": {".py"},
    "backend/application/observability/catalog": {".def"},
    "docs/contracts": {".json"},
    "config/observability/dashboards": {".json"},
    "backend/application/storage/postgres/migrations": {".sql"},
    "backend/study/AccountWorkspace": {".py"},
    "backend/agent/tool_runtime": {".py"},
    "backend/agent/tools": {".py", ".md"},
    "backend/agent/skills": {".py", ".md"},
    "client/yantu/runtime": {".py", ".ps1", ".txt", ".html", ".js", ".mjs", ".css", ".webmanifest",
                             ".json", ".png", ".woff", ".woff2", ".ttf", ".pfb", ".bcmap", ".icc", ".wasm", ".svg", ".gif"},
    "config/gateway": {".json"},
}
FLEET_PROGRAMS = {
    "fleet/nebula_fleet_coordinator": "fleet/transport/grpc/nebula_fleet_coordinator",
    "fleet/nebula_fleet_edge": "fleet/transport/grpc/nebula_fleet_edge",
    "fleet/nebula_fleet_ctl": "fleet/transport/grpc/nebula_fleet_ctl",
    "fleet/nebula_fleet_agent": "agent/nebula_fleet_agent",
}
FLEET_EVIDENCE = "fleet/cpp-systems-evidence.json"
RUNTIME_ENTRY_FILES = {"backend/agent/catalog.json", "tools/observability/gateway.py", "tools/observability/gateways/__init__.py",
                       "tools/observability/gateways/gateway.py", "prepare-runtime-libs.py", *FLEET_PROGRAMS, FLEET_EVIDENCE}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def as_json(value):
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def runtime_path(name):
    if name in RUNTIME_ENTRY_FILES:
        return True
    if name in {"webserver", "reproduction.json", "LICENSE", "release_preflight.py", "run-nebula.py",
                "native-runtime.json", "workers/webserver-onnx-worker", "workers/function3-tts-worker"}:
        return True
    path = PurePosixPath(name)
    return (name == path.as_posix() and not path.is_absolute() and
            all(not part.startswith(".") and "\\" not in part and ":" not in part for part in path.parts) and
            any(path.is_relative_to(folder) and
                (path.suffix in suffixes or (folder in {"frontend/phase11", "client/yantu/runtime"} and path.name.startswith("LICENSE")))
                for folder, suffixes in RUNTIME_FILES.items()))


def validate_reproduction(report, binary_hash, revision):
    require(report.get("passed") is True and report.get("profile") in ("business","native"),
            "A successful business reproduction is required")
    require(report.get("binarySha256") == binary_hash, "Business evidence belongs to another binary")
    if report['profile']=='native':
        native=report.get('nativeInventory',{})
        require(native.get('passed') is True and native.get('profile')=='native'
                and all(native.get('features',{}).get(key) is True for key in ('GRPC','ONNX_GENAI','ROCKETMQ','SHERPA_TTS')),
                'Native feature inventory is incomplete')
        require(set(native.get('programs',{}))=={'server','onnx','speech'} and
                native['programs']['server']['sha256']==binary_hash and
                all(row.get('linkage',{}).get('resolved') is True for row in native['programs'].values()),
                'Native worker or runtime library evidence is incomplete')
    smoke = report.get("smoke", {})
    wire = report.get("otlpWire", {})
    require(smoke.get("schemaVerifiedAtStartup") is True and report.get("migration") is not None
            and smoke.get("requiredMigration") == report["migration"] == wire.get("schema"),
            "Runtime schema, migration and wire check must agree")
    ctest = report.get("ctest", {})
    require(ctest.get("passed", 0) > 0 and
            ctest.get("registered") == ctest["passed"] + len(ctest.get("skipped", ())) and
            set(ctest.get("skipped", ())) <= {"function3_redis_contract"},
            "CTest evidence is incomplete")
    require((wire.get("unauthenticated"), wire.get("nonOps"), wire.get("ops")) == (401, 403, 200)
            and wire.get("postgreSqlSpans", 0) > 0 and wire.get("exportResumed") is True
            and wire.get("httpSampling", {}).get("failureRoots") == 1,
            "A successful OTLP wire, fault recovery and HTTP sampling check is required")
    if revision:
        require(re.fullmatch(r"[0-9a-f]{40}", revision) and smoke.get("revision") == revision[:12],
                "Business evidence belongs to another Git revision")


def validate_fleet(report, digest=None):
    require(report.get("passed") is True and report.get("phase") == "complete"
            and report.get("config") == "Release" and report.get("grpc") is True
            and report.get("sanitizer") == "none" and report.get("nav2") is False
            and {"nebula_fleet_process_contract", "nebula_fleet_backpressure", "nebula_agent_approval_handoff",
                 "nebula_simulation_models_and_replay"} <= set(report.get("tests", ())),
            "A successful Release gRPC fleet check is required")
    require(digest is None or report["sourceSha256"] == digest, "Fleet evidence belongs to another source tree")


def runtime_payload(build, fleet_build=None):
    payload = {"webserver": (build / "webserver").read_bytes(),
               "reproduction.json": (build / "reproduction.json").read_bytes(),
               "LICENSE": (ROOT / "LICENSE").read_bytes(),
               "release_preflight.py":(ROOT/'scripts/project/release_preflight.py').read_bytes(),
               "run-nebula.py":(ROOT/'scripts/project/run_release.py').read_bytes()}
    if fleet_build is not None:
        evidence = (fleet_build / "cpp-systems-evidence.json").read_bytes()
        validate_fleet(json.loads(evidence), fleet_source_digest())
        payload[FLEET_EVIDENCE] = evidence
        for name, path in FLEET_PROGRAMS.items():
            binary = (fleet_build / path).read_bytes()
            require(binary.startswith(b"\x7fELF"), f"Fleet entry is not a Linux ELF program: {name}")
            payload[name] = binary
    report=json.loads(payload['reproduction.json'])
    if report.get('profile')=='native':
        native=json.loads(json.dumps(report['nativeInventory']))
        for name,row in native['programs'].items():
            relative=row['path'];source=(build/relative).resolve()
            require(source.is_relative_to(build.resolve()) and source.is_file(),'Native worker path invalid')
            require(sha256(source.read_bytes())==row['sha256'],'Native worker differs from tested build')
            target='webserver' if name=='server' else 'workers/'+source.name
            payload[target]=source.read_bytes();row['path']=target
        payload['native-runtime.json']=as_json(native)
    for folder in RUNTIME_FILES:
        for file in (ROOT / folder).rglob("*"):
            relative = file.relative_to(ROOT)
            if runtime_path(relative.as_posix()):
                require(file.is_file() and not file.is_symlink(), f"Invalid runtime file: {relative}")
                payload[relative.as_posix()] = file.read_bytes()
    for name in RUNTIME_ENTRY_FILES:
        if name in FLEET_PROGRAMS or name in (FLEET_EVIDENCE, "prepare-runtime-libs.py"):
            continue
        file = ROOT / name
        require(file.is_file() and not file.is_symlink(), f"Invalid runtime entry: {name}")
        payload[name] = file.read_bytes()
    payload["prepare-runtime-libs.py"] = (ROOT / "scripts/project/runtime_libs.py").read_bytes()
    return payload


def members(archive):
    if archive.suffix.lower() == ".zip":
        with zipfile.ZipFile(archive) as bundle:
            for item in bundle.infolist():
                mode = item.external_attr >> 16
                require(stat.S_ISREG(mode), f"Release member is not a regular file: {item.filename}")
                yield item.filename, stat.S_IMODE(mode), bundle.read(item)
    else:
        with tarfile.open(archive, "r:gz") as bundle:
            for item in bundle.getmembers():
                require(item.isfile(), f"Release member is not a regular file: {item.name}")
                with bundle.extractfile(item) as data:
                    yield item.name, item.mode, data.read()


def verified_payload(archive, revision):
    payload = {}
    for name, mode, data in members(archive):
        require(name == "release.json" or runtime_path(name), "Release archive has a non-runtime path")
        require(name not in payload, "Release archive has unexpected or repeated paths")
        payload[name] = (mode, data)
    manifest = json.loads(payload["release.json"][1])
    require(manifest.get("schemaVersion") in (1, 2), "Unsupported release manifest version")
    files = manifest["files"]
    require(payload.keys() == files.keys() | {"release.json"}, "Release archive has unexpected or repeated paths")
    for name, expected in files.items():
        require(sha256(payload[name][1]) == expected, f"Changed release member: {name}")
    require(payload["webserver"][0] == 0o755, "Server is not executable")
    require(revision is None or manifest["revision"] == revision, "Release manifest revision does not match")
    report = json.loads(payload["reproduction.json"][1])
    validate_reproduction(report, files["webserver"], revision)
    if manifest["schemaVersion"] == 2:
        require({"release_preflight.py", "run-nebula.py"} <= files.keys(), "Release preflight is missing")
    fleet_files = FLEET_PROGRAMS.keys() | {FLEET_EVIDENCE}
    if fleet_files & files.keys():
        require(fleet_files <= files.keys(), "Fleet payload is incomplete")
        validate_fleet(json.loads(payload[FLEET_EVIDENCE][1]))
        require(all(payload[name][0] == 0o755 for name in FLEET_PROGRAMS), "Fleet program is not executable")
    if report["profile"] == "native":
        required = {"native-runtime.json", "workers/webserver-onnx-worker", "workers/function3-tts-worker"}
        require(required <= files.keys(), "Native runtime payload is missing")
        native = json.loads(payload["native-runtime.json"][1])
        require(set(native.get("programs", {})) == {"server", "onnx", "speech"}, "Native programs are incomplete")
        for name, row in native["programs"].items():
            tested = report["nativeInventory"]["programs"][name]
            expected = "webserver" if name == "server" else "workers/" + Path(tested["path"]).name
            require(row["path"] == expected and files.get(expected) == row["sha256"] == tested["sha256"],
                    "Native manifest differs from tested programs")
            require(payload[expected][0] == 0o755, "Native program is not executable")
    return manifest, payload


def verify_archive(archive, revision):
    return verified_payload(archive, revision)[0]


def executable_mode(name):
    return 0o755 if name == "webserver" or name in FLEET_PROGRAMS or name.startswith("workers/") else 0o644


def write_archive(archive, payload):
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive, "x") as bundle:
            for name, data in sorted(payload.items()):
                item = zipfile.ZipInfo(name)
                item.create_system = 3
                item.external_attr = (stat.S_IFREG | executable_mode(name)) << 16
                bundle.writestr(item, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
    else:
        with archive.open("xb") as raw, gzip.GzipFile(fileobj=raw, filename="", mode="wb", mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as bundle:
                for name, data in sorted(payload.items()):
                    member = tarfile.TarInfo(name)
                    member.size = len(data)
                    member.mode = executable_mode(name)
                    bundle.addfile(member, io.BytesIO(data))


def extract_payload(archive, destination, revision=None):
    manifest, payload = verified_payload(archive, revision)
    destination.mkdir(parents=True, exist_ok=False)
    for name, (mode, data) in payload.items():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(mode & 0o777)
    return manifest


def package(build, output, revision, archive_format="tar.gz", fleet_build=None):
    payload = runtime_payload(build, fleet_build)
    report = json.loads(payload["reproduction.json"])
    binary_hash = sha256(payload["webserver"])
    validate_reproduction(report, binary_hash, revision)
    smoke = report["smoke"]
    require(smoke.get("sourceFingerprint") == fingerprint(), "Source bytes differ from the tested build")
    require(json.loads(payload["frontend/phase11/asset-manifest.json"])["version"] == smoke.get("assetVersion"),
            "Frontend asset manifest differs from the tested version")
    require(all(name in payload for name in ("frontend/observability/index.html", "tools/observability/observer/http/server.py")),
            "Observer runtime is missing")
    require(all(name in payload for name in ("backend/study/AccountWorkspace/AccountWorkspace.py",
                                             "client/yantu/runtime/target/index.html", "config/gateway/envoy.json")),
            "Yantu account runtime is missing")
    manifest = {"schemaVersion": 2, "profile":report['profile'],"revision": revision or smoke["revision"], "machine": platform.machine(),
                "sourceFingerprint": smoke["sourceFingerprint"], "assetVersion": smoke["assetVersion"],
                "migration": report["migration"], "files": {name: sha256(data) for name, data in payload.items()}}
    payload["release.json"] = as_json(manifest)
    output.mkdir(parents=True, exist_ok=False)
    archive = output / f"nebula-linux-{platform.machine()}.{archive_format}"
    write_archive(archive, payload)
    verify_archive(archive, revision)
    (output / "release.json").write_bytes(payload["release.json"])
    (output / "SHA256SUMS").write_text(f"{sha256(archive.read_bytes())}  {archive.name}\n", encoding="ascii")
    print(archive)


def smoke_archive(archive, manifest, postgres_prefix):
    """Run from the extracted runtime tree with a new disposable database."""
    require(sys.platform.startswith("linux"), "The packaged server requires Linux")
    with tempfile.TemporaryDirectory(prefix="nebula-delivery-") as directory:
        work = Path(directory)
        source = work / "runtime"
        require(extract_payload(archive, source) == manifest, "Archive changed after verification")
        key = work / "token.key"
        key.touch(mode=0o600, exist_ok=False)
        key.write_text(secrets.token_hex(32), encoding="ascii")
        with disposable_database(postgres_prefix.resolve(), source, work) as (connection, migration):
            env = {**clean_environment(), "WEB_PHASE11_REQUIRED": "1", "WEB_FUNCTION3_ENABLE": "1",
                   "WEB_PHASE11_DATABASE": connection, "WEB_PHASE11_TOKEN_KEY_FILE": str(key),
                   "WEB_PHASE11_FILE_ROOT": str(work / "files"), "WEB_FUNCTION3_MEDIA_ROOT": str(work / "media")}
            with owned_server(source / "webserver", source, env, work / "server.log") as port:
                observed = http_smoke(source, port, True)
            require(observed["sourceFingerprint"] == manifest["sourceFingerprint"] and
                    observed["assetVersion"] == manifest["assetVersion"] and
                    observed["requiredMigration"] == migration == manifest["migration"],
                    "Packaged runtime does not agree with the delivery manifest")
        subprocess.run([sys.executable, "-m", "tools.observability.observer.http.server", "serve", "--help"],
                       cwd=source, env=clean_environment(), check=True, stdout=subprocess.DEVNULL)
        if manifest.get('schemaVersion')==2:
            subprocess.run([sys.executable,str(source/'release_preflight.py'),'--root',str(source)],
                           cwd=source,env=clean_environment(),check=True,stdout=subprocess.DEVNULL)
    print("Staged server, PostgreSQL schema, frontend assets and Observer entry point verified")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", help="Expected full Git commit SHA")
    mode = parser.add_subparsers(dest="mode", required=True)
    create = mode.add_parser("package")
    create.add_argument("--build", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--format", choices=("tar.gz", "zip"), default="tar.gz")
    create.add_argument("--fleet-build", type=Path, help="Optional verified independent Linux C++ Fleet build")
    check = mode.add_parser("verify")
    check.add_argument("archive", type=Path)
    check.add_argument("--postgres-prefix", type=Path, help="Also unpack and smoke-test with a disposable database")
    args = parser.parse_args()
    if args.mode == "package":
        package(args.build, args.output, args.revision, args.format, args.fleet_build)
    else:
        manifest = verify_archive(args.archive, args.revision)
        print(f"Verified {len(manifest['files'])} files for revision {manifest['revision']}")
        if args.postgres_prefix:
            smoke_archive(args.archive, manifest, args.postgres_prefix)


if __name__ == "__main__":
    main()
