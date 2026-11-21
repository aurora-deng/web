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
import subprocess
import sys
import tarfile
import tempfile

from build_stamp import fingerprint
from repro_runtime import clean_environment, disposable_database, http_smoke, owned_server, require

ROOT = Path(__file__).resolve().parents[2]

# Only application runtime files are delivered. Local keys, build logs, test
# databases and model files are outside the allowlisted paths and formats.
RUNTIME_FILES = {
    "frontend/phase11": {".html", ".css", ".js", ".json", ".svg", ".webp", ".png", ".jpg", ".jpeg", ".woff2", ".txt"},
    "frontend/observability": {".html", ".css", ".js", ".svg"},
    "static": {".svg"},
    "tools/observability/observer": {".py"},
    "backend/application/observability/OperationCatalog": {".def"},
    "docs/contracts": {".json"},
    "config/observability/dashboards": {".json"},
    "backend/application/storage/postgres/migrations": {".sql"},
    "backend/study/AccountWorkspace": {".py"},
    "client/yantu/runtime": {".py", ".ps1", ".txt", ".html", ".js", ".mjs", ".css", ".webmanifest",
                             ".json", ".png", ".woff", ".woff2", ".ttf", ".pfb", ".bcmap", ".icc", ".wasm", ".svg", ".gif"},
    "config/gateway": {".json"},
}
RUNTIME_ENTRY_FILES = {"tools/observability/gateway.py"}


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


def runtime_payload(build):
    payload = {"webserver": (build / "webserver").read_bytes(),
               "reproduction.json": (build / "reproduction.json").read_bytes(),
               "LICENSE": (ROOT / "LICENSE").read_bytes(),
               "release_preflight.py":(ROOT/'scripts/project/release_preflight.py').read_bytes(),
               "run-nebula.py":(ROOT/'scripts/project/run_release.py').read_bytes()}
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
        file = ROOT / name
        require(file.is_file() and not file.is_symlink(), f"Invalid runtime entry: {name}")
        payload[name] = file.read_bytes()
    return payload


def verify_archive(archive, revision):
    with tarfile.open(archive, "r:gz") as bundle:
        names = bundle.getnames()
        require(all(name == "release.json" or runtime_path(name) for name in names),
                "Release archive has a non-runtime path")
        manifest = json.load(bundle.extractfile("release.json"))
        require(manifest.get('schemaVersion') in (1,2),'Unsupported release manifest version')
        files = manifest["files"]
        require(len(names) == len(files) + 1 and set(names) == files.keys() | {"release.json"},
                "Release archive has unexpected or repeated paths")
        for member in bundle.getmembers():
            require(member.isfile(), f"Release member is not a regular file: {member.name}")
            if member.name in files:
                actual = sha256(bundle.extractfile(member).read())
                require(actual == files[member.name], f"Changed release member: {member.name}")
        require(bundle.getmember("webserver").mode == 0o755, "Server is not executable")
        require(revision is None or manifest["revision"] == revision, "Release manifest revision does not match")
        report = json.load(bundle.extractfile("reproduction.json"))
        validate_reproduction(report, files["webserver"], revision)
        if manifest.get('schemaVersion')==2:
            require({'release_preflight.py','run-nebula.py'} <= files.keys(),'Release preflight is missing')
        if report['profile']=='native':
            required={'native-runtime.json','workers/webserver-onnx-worker','workers/function3-tts-worker'}
            require(required <= files.keys(),'Native runtime payload is missing')
            native=json.load(bundle.extractfile('native-runtime.json'))
            require(set(native.get('programs',{}))=={'server','onnx','speech'},'Native programs are incomplete')
            for name,row in native['programs'].items():
                expected='webserver' if name=='server' else 'workers/'+Path(report['nativeInventory']['programs'][name]['path']).name
                require(row['path']==expected and files.get(expected)==row['sha256']==report['nativeInventory']['programs'][name]['sha256'],
                        'Native manifest differs from tested programs')
                require(bundle.getmember(expected).mode==0o755,'Native program is not executable')
    return manifest


def package(build, output, revision):
    payload = runtime_payload(build)
    report = json.loads(payload["reproduction.json"])
    binary_hash = sha256(payload["webserver"])
    validate_reproduction(report, binary_hash, revision)
    smoke = report["smoke"]
    require(smoke.get("sourceFingerprint") == fingerprint(), "Source bytes differ from the tested build")
    require(json.loads(payload["frontend/phase11/asset-manifest.json"])["version"] == smoke.get("assetVersion"),
            "Frontend asset manifest differs from the tested version")
    require(all(name in payload for name in ("frontend/observability/index.html", "tools/observability/observer/server.py")),
            "Observer runtime is missing")
    require(all(name in payload for name in ("backend/study/AccountWorkspace/AccountWorkspace.py",
                                             "client/yantu/runtime/target/index.html", "config/gateway/envoy.json")),
            "Yantu account runtime is missing")
    manifest = {"schemaVersion": 2, "profile":report['profile'],"revision": revision or smoke["revision"], "machine": platform.machine(),
                "sourceFingerprint": smoke["sourceFingerprint"], "assetVersion": smoke["assetVersion"],
                "migration": report["migration"], "files": {name: sha256(data) for name, data in payload.items()}}
    payload["release.json"] = as_json(manifest)
    output.mkdir(parents=True, exist_ok=False)
    archive = output / f"nebula-linux-{platform.machine()}.tar.gz"
    with archive.open("wb") as raw, gzip.GzipFile(fileobj=raw, filename="", mode="wb", mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w") as bundle:
            for name, data in sorted(payload.items()):
                member = tarfile.TarInfo(name)
                member.size = len(data)
                member.mode = 0o755 if name == "webserver" or name.startswith('workers/') else 0o644
                bundle.addfile(member, io.BytesIO(data))
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
        source.mkdir()
        with tarfile.open(archive, "r:gz") as bundle:
            for member in bundle.getmembers():
                require(member.isfile() and (member.name == "release.json" or runtime_path(member.name)),
                        f"Invalid release member: {member.name}")
                data = bundle.extractfile(member).read()
                require(member.name == "release.json" or sha256(data) == manifest["files"].get(member.name),
                        f"Changed release member: {member.name}")
                target = source / member.name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
                target.chmod(member.mode & 0o777)
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
        subprocess.run([sys.executable, "-m", "tools.observability.observer.server", "serve", "--help"],
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
    check = mode.add_parser("verify")
    check.add_argument("archive", type=Path)
    check.add_argument("--postgres-prefix", type=Path, help="Also unpack and smoke-test with a disposable database")
    args = parser.parse_args()
    if args.mode == "package":
        package(args.build, args.output, args.revision)
    else:
        manifest = verify_archive(args.archive, args.revision)
        print(f"Verified {len(manifest['files'])} files for revision {manifest['revision']}")
        if args.postgres_prefix:
            smoke_archive(args.archive, manifest, args.postgres_prefix)


if __name__ == "__main__":
    main()
