"""Build the isolated C++ systems targets and retain machine-readable test evidence."""
import argparse
from hashlib import sha256
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SANITIZERS = {"none": "", "tsan": "thread", "asan-ubsan": "address,undefined"}


def source_digest():
    digest = sha256()
    for part in ("backend/systems", "backend/workflow", "backend/fleet", "backend/agent", "robotics/nav2_gateway",
                 "robotics/actuator_control", "robotics/actuator_ros_sim",
                 "tests/cpp_systems", "benchmarks/cpp_systems", "tools/agent", "tools/body"):
        paths = sorted((ROOT / part).rglob("*"), key=lambda path: path.relative_to(ROOT).parts)
        for path in paths:
            if path.suffix in (".txt", ".h", ".cpp", ".proto", ".py"):
                digest.update(path.relative_to(ROOT).as_posix().encode() + b"\0")
                digest.update(path.read_bytes().replace(b"\r\n", b"\n"))
                digest.update(b"\0")
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", choices=("Debug", "Release"), default="Release")
    parser.add_argument("--sanitizer", choices=SANITIZERS, default="none")
    parser.add_argument("--with-grpc", action="store_true")
    parser.add_argument("--with-nav2", action="store_true", help="Build and exercise ROS 2 in a sourced Linux environment")
    parser.add_argument("--benchmarks", action="store_true", help="Build standalone DAG and optional Fleet benchmarks")
    parser.add_argument("--prefix", type=Path, help="Already installed gRPC/Protobuf prefix")
    parser.add_argument("--require-test", action="append", default=[], help="Require an extra named CTest test")
    args = parser.parse_args()
    if args.sanitizer != "none" and not sys.platform.startswith("linux"):
        parser.error("this sanitizer profile requires a supported Linux compiler and runtime")
    if args.prefix and (not args.with_grpc or not args.prefix.is_dir()):
        parser.error("the existing prefix directory is required only with --with-grpc")
    if args.with_nav2 and (not args.with_grpc or not sys.platform.startswith("linux")):
        parser.error("Nav2 validation requires Linux and --with-grpc")

    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    evidence = {
        "config": args.config,
        "sanitizer": args.sanitizer,
        "grpc": args.with_grpc,
        "nav2": args.with_nav2,
        "benchmarks": args.benchmarks,
        "platform": platform.platform(),
        "sourceSha256": source_digest(),
        "tests": [],
        "passed": False,
    }
    env = dict(os.environ)
    flags = ""
    if args.sanitizer != "none":
        flags = f"-fsanitize={SANITIZERS[args.sanitizer]} -fno-omit-frame-pointer"
        env.update(TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1",
                   ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                   UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
    if args.prefix and os.name == "nt":
        env["PATH"] = str(args.prefix.resolve() / "bin") + os.pathsep + env["PATH"]
    cache = [
        f"-DCMAKE_BUILD_TYPE={args.config}",
        "-DBUILD_TESTING=ON",
        "-DNEBULA_ACTUATOR_SIM=ON",
        "-DNEBULA_ENABLE_FLEET=ON",
        f"-DNEBULA_FLEET_GRPC={'ON' if args.with_grpc else 'OFF'}",
        f"-DNEBULA_FLEET_NAV2={'ON' if args.with_nav2 else 'OFF'}",
        f"-DNEBULA_WORKFLOW_BENCHMARK={'ON' if args.benchmarks else 'OFF'}",
        f"-DNEBULA_FLEET_BENCHMARK={'ON' if args.benchmarks and args.with_grpc else 'OFF'}",
        f"-DCMAKE_PREFIX_PATH={args.prefix.resolve() if args.prefix else ''}",
    ]
    if sys.platform.startswith("linux"):
        cache.append(f"-DCMAKE_CXX_FLAGS={flags}")
    try:
        evidence["phase"] = "configure"
        subprocess.run(["cmake", "-S", str(ROOT / "backend/systems"), "-B", str(build), *cache],
                       check=True, env=env)
        evidence["phase"] = "build"
        subprocess.run(["cmake", "--build", str(build), "--config", args.config, "--parallel", "2"],
                       check=True, env=env)
        evidence["phase"] = "test-discovery"
        label = "cpp-systems|actuator-sim|ros2-sim" if args.with_nav2 else "cpp-systems|actuator-sim"
        ctest = ["ctest", "--test-dir", str(build), "-C", args.config, "-L", label]
        discovered = json.loads(subprocess.check_output([*ctest, "--show-only=json-v1"], env=env))
        names = {entry["name"] for entry in discovered["tests"]}
        evidence["tests"] = sorted(names)
        required = {"nebula_workflow_contract", "nebula_workflow_recovery", "nebula_fleet_contract",
                    "nebula_agent_contract", "nebula_actuator_contract"}
        if args.with_grpc:
            required.update({"nebula_fleet_process_contract", "nebula_fleet_backpressure", "nebula_agent_approval_handoff",
                             "nebula_agent_model_adapter", "nebula_simulation_models_and_replay"})
        if args.with_nav2:
            required.update({"nebula_nav2_contract", "nebula_nav2_fleet_process"})
        required.update(args.require_test)
        if required - names:
            raise RuntimeError(f"missing native tests: {sorted(required - names)}")
        evidence["phase"] = "ctest"
        subprocess.run([*ctest, "--no-tests=error", "--output-on-failure",
                        "--output-junit", str(build / "ctest.xml")], check=True, env=env)
        evidence["passed"] = True
        evidence["phase"] = "complete"
    finally:
        (build / "cpp-systems-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
