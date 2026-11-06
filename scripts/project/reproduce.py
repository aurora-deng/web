"""Configure, build and smoke-test Nebula in a fresh Linux directory without downloads."""
import argparse
from contextlib import nullcontext
import hashlib
import json
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

from ci_checks import test_matrix
from repro_runtime import (browser_fixture, business_smoke, clean_environment,
                           disposable_database, http_smoke, owned_server, require)

ROOT = Path(__file__).resolve().parents[2]


def cache_values(path):
    return dict((line.split(":", 1)[0], line.split("=", 1)[1])
                for line in path.read_text().splitlines()
                if "=" in line and not line.startswith(("//", "#")))


def run_build(args, build, business):
    command = ["cmake", "-S", str(ROOT), "-B", str(build), "-G", args.generator,
               "-DCMAKE_BUILD_TYPE=Release", "-DWEBSERVER_ENABLE_PHASE11=ON"]
    on = "ON" if business else "OFF"
    command += [f"-D{key}={on}" for key in
                ("BUILD_TESTING", "WEBSERVER_ENABLE_POSTGRES", "WEBSERVER_ENABLE_SODIUM")]
    for key, value in (("PostgreSQL_ROOT", args.postgres_prefix),
                       ("WEBSERVER_SODIUM_ROOT", args.sodium_prefix), ("GTest_DIR", args.gtest_dir),
                       ("OPENSSL_INCLUDE_DIR", args.openssl_include)):
        if value:
            command.append(f"-D{key}={value.resolve()}")
    if args.openssl_lib_dir:
        for part in ("ssl", "crypto"):
            library = args.openssl_lib_dir.resolve() / f"lib{part}.so"
            require(library.is_file(), f"OpenSSL library missing: {library}")
            command.append(f"-DOPENSSL_{part.upper()}_LIBRARY={library}")
    subprocess.run(command, check=True)
    subprocess.run(["cmake", "--build", str(build), "--parallel", str(args.jobs)], check=True)


def verify_config(build, business):
    cache = cache_values(build / "CMakeCache.txt")
    require(Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() == ROOT, "Build uses another checkout")
    require(cache["CMAKE_BUILD_TYPE"] == "Release", "Reproduction requires a Release build")
    expected = "ON" if business else "OFF"
    for flag in ("BUILD_TESTING", "WEBSERVER_ENABLE_POSTGRES", "WEBSERVER_ENABLE_SODIUM"):
        require(cache.get(flag) == expected, f"{flag} must be {expected}")
    for option in ("GRPC", "CURL_CLIENT", "ONNX_GENAI", "ROCKETMQ", "SHERPA_TTS", "ASAN", "UBSAN", "TSAN"):
        require(cache.get("WEBSERVER_ENABLE_" + option, "OFF") == "OFF",
                f"Reference profile requires {option} off")
    return {"buildType": "Release", "generator": cache["CMAKE_GENERATOR"],
            "compiler": subprocess.check_output([cache["CMAKE_CXX_COMPILER"], "--version"],
                                                text=True).splitlines()[0],
            "cmake": subprocess.check_output(["cmake", "--version"], text=True).splitlines()[0]}


def verify_ctest(build, connection):
    tests = json.loads(subprocess.check_output(
        ["ctest", "--test-dir", str(build), "--show-only=json-v1"], text=True))["tests"]
    lanes = test_matrix([case["name"] for case in tests])
    env = {**clean_environment(), "PHASE11_POSTGRES_CONNECTION": connection}
    with tempfile.TemporaryDirectory(prefix="nebula-ctest-") as scratch:
        junit = Path(scratch) / "ctest.xml"
        subprocess.run(["ctest", "--test-dir", str(build), "--output-on-failure",
                        "--no-tests=error", "--output-junit", str(junit), "-j", "1"],
                       check=True, env=env)
        cases = ET.parse(junit).findall(".//testcase")
    skipped = [case.attrib["name"] for case in cases if case.find("skipped") is not None]
    require(len(cases) == len(tests), "CTest report does not cover the discovered matrix")
    require(set(skipped) <= {"function3_redis_contract"},
            f"Unexpected skipped business/protocol tests: {skipped}")
    return {"registered": len(tests), "passed": len(cases) - len(skipped),
            "skipped": skipped, "lanes": {name: len(items) for name, items in lanes.items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", choices=("transport", "business"))
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--verify-existing-build", action="store_true",
                        help="Use an explicitly selected, matching pre-built checkout")
    parser.add_argument("--generator", default="Unix Makefiles", choices=("Unix Makefiles", "Ninja"))
    parser.add_argument("--jobs", type=int, choices=range(1, 9), default=2)
    parser.add_argument("--postgres-prefix", type=Path, help="Installed PostgreSQL prefix with initdb and psql")
    parser.add_argument("--sodium-prefix", type=Path, help="Already installed libsodium prefix")
    parser.add_argument("--gtest-dir", type=Path, help="Optional installed GTest CMake config directory")
    parser.add_argument("--openssl-include", type=Path, help="Explicit OpenSSL headers if discovery is ambiguous")
    parser.add_argument("--openssl-lib-dir", type=Path, help="Directory with the matching libssl.so and libcrypto.so")
    args = parser.parse_args()
    if not sys.platform.startswith("linux"):
        parser.error("Linux server only; use the source checks and Windows client build on Windows")
    business = args.profile == "business"
    if business and not args.postgres_prefix:
        parser.error("business requires --postgres-prefix; no existing database DSN is accepted")
    if bool(args.openssl_include) != bool(args.openssl_lib_dir):
        parser.error("Specify both OpenSSL locations together")
    for command in ("cmake", "ctest"):
        if not shutil.which(command):
            parser.error(f"Missing existing command: {command}")
    build = args.build_dir.resolve()
    if args.verify_existing_build:
        require((build / "CMakeCache.txt").is_file(), "No existing CMake configuration")
    else:
        require(not build.exists() or (build.is_dir() and not any(build.iterdir())),
                "Choose a new or empty build directory")
        build.mkdir(parents=True, exist_ok=True)
    report = {"profile": args.profile, "passed": False, "installedDependencies": False,
              "existingDatabaseUsed": False, "builtFromEmptyDirectory": not args.verify_existing_build}
    try:
        if not args.verify_existing_build:
            run_build(args, build, business)
        report["build"] = verify_config(build, business)
        binary = build / "webserver"
        report["binarySha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
        database = (disposable_database(args.postgres_prefix.resolve(), ROOT, build)
                    if business else nullcontext(("", None)))
        with database as (connection, migration), tempfile.TemporaryDirectory(prefix="nebula-runtime-") as work:
            env = clean_environment()
            if business:
                report["migration"] = migration
                report["ctest"] = verify_ctest(build, connection)
                key = Path(work) / "token.key"
                key.touch(mode=0o600, exist_ok=False)
                key.write_text(secrets.token_hex(32), encoding="ascii")
                env.update(WEB_PHASE11_REQUIRED="1", WEB_FUNCTION3_ENABLE="1",
                           WEB_PHASE11_DATABASE=connection, WEB_PHASE11_TOKEN_KEY_FILE=str(key),
                           WEB_PHASE11_FILE_ROOT=str(Path(work) / "files"),
                           WEB_FUNCTION3_MEDIA_ROOT=str(Path(work) / "media"))
            with owned_server(binary, ROOT, env, build / "smoke.log") as port:
                report["smoke"] = http_smoke(ROOT, port, business)
                if business:
                    require(report["smoke"]["requiredMigration"] == migration,
                            "Build expects a different migration from the applied ledger")
                    client, post = business_smoke(port, browser_fixture(ROOT))
                    report["checks"] = ["registration", "login", "WebSocket upgrade",
                                        "story creation", "idempotency", "durable Outbox SSE"]
            if business:
                with owned_server(binary, ROOT, env, build / "restart.log") as port:
                    client.port = port
                    status, found = client.request("GET", "/api/tree/posts/" + post)
                    require(status == 200 and str(found["id"]) == post,
                            "Story or session not preserved after restart")
                report["checks"].append("session and story recovered after restart")
        report["passed"] = True
    finally:
        (build / "reproduction.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
