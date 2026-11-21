"""Run the dual-instance chat acceptance test in a loopback-only Linux network."""

import argparse
import errno
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import time


PARENT_NETWORK = "NEBULA_LAB_PARENT_NETWORK"


def require_isolated_network():
    parent = os.environ.get(PARENT_NETWORK)
    if not parent:
        raise RuntimeError("Start the dual-instance lab with tools.observability.isolated_dual")
    current = os.readlink("/proc/self/ns/net")
    interfaces = sorted(name for _, name in socket.if_nameindex())
    if current == parent or interfaces != ["lo"]:
        raise RuntimeError("Dual-instance testing requires a separate, loopback-only network namespace")
    return {"hostNamespace": parent, "labNamespace": current, "interfaces": interfaces}


def wait_for_readiness(lab, directory, timeout=150):
    marker = directory / "ready.pid"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if lab.poll() is not None:
            raise RuntimeError(f"Lab exited with status {lab.returncode} before readiness")
        if marker.exists() and marker.read_text(encoding="ascii").strip() == str(lab.pid):
            return
        time.sleep(0.5)
    raise RuntimeError("Timed out waiting for the isolated monitoring services")


def stop_lab(lab):
    if lab.poll() is None:
        lab.terminate()
    status = lab.wait(timeout=120)
    if status != 0:
        raise RuntimeError(f"Isolated lab exited with status {status}")


def verify_loopback_isolation(host_port):
    network = require_isolated_network()
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.settimeout(2)
        if probe.connect_ex(("127.0.0.1", host_port)) != errno.ECONNREFUSED:
            raise RuntimeError("Could not verify that the host loopback listener is isolated")
    network["hostLoopbackIsolated"] = True
    return network


def exercise(args):
    network = verify_loopback_isolation(args.host_probe_port)
    report = args.work_dir / "isolation.json"
    report.write_text(json.dumps(network, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"networkIsolation": network}), flush=True)
    directory = args.work_dir / "lab"
    start = [sys.executable, "-m", "tools.observability.lab", "--dual-node", "--allow-primary-crash",
             "--profile", args.profile, "--dependencies", str(args.dependencies),
             "--postgres-prefix", str(args.postgres_prefix), "--binary", str(args.binary),
             "--work-dir", str(directory), "--http-trace-every", "1", "--ws-trace-every", "1", "--duration", "420",
             "--data-budget-mib", "384"]
    verify = [sys.executable, "-m", "tools.observability.verify_dual_chat", "--run-dir", str(directory),
              "--postgres-prefix", str(args.postgres_prefix), "--trace", "--crash-primary",
              "--output", str(directory / "exports/dual-chat.json")]
    if args.with_alertmanager:
        start.append("--with-alertmanager")
        verify.append("--notifications")
    if args.profile == "full":
        verify.append("--collector-outage")
    if args.gateway_envoy:
        start.extend(("--gateway-envoy", str(args.gateway_envoy.resolve())))
        verify.append("--through-gateway")

    def interrupted(signum, _frame):
        raise SystemExit(128 + signum)

    for signum in (signal.SIGHUP, signal.SIGTERM):
        signal.signal(signum, interrupted)
    with (args.work_dir / "lab-console.log").open("xb") as log:
        lab = subprocess.Popen(start, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_for_readiness(lab, directory)
            print(f"DUAL_LAB_READY={args.profile}", flush=True)
            subprocess.run(verify, stdin=subprocess.DEVNULL, check=True, timeout=380)
        except BaseException:
            try:
                stop_lab(lab)
            except (RuntimeError, subprocess.TimeoutExpired) as error:
                print(f"Lab cleanup: {error}", file=sys.stderr)
            raise
        else:
            stop_lab(lab)
    print(f"DUAL_ACCEPTANCE_PASSED={args.profile}", flush=True)


def enter_namespace(args, original, *, module="tools.observability.isolated_dual"):
    programs = {name: shutil.which(name) for name in ("unshare", "ip", "setpriv")}
    missing = [name for name, path in programs.items() if path is None]
    if missing:
        raise RuntimeError(f"Linux namespace tools unavailable: {', '.join(missing)}")
    args.work_dir.mkdir(mode=0o700, parents=True, exist_ok=False)
    parent = os.readlink("/proc/self/ns/net")
    env = {**os.environ, PARENT_NETWORK: parent}
    # A private net namespace starts with lo down. Drop the setup capabilities before running any test code.
    setup = 'set -eu; "$1" link set dev lo up; shift; exec "$@"'
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        completed = subprocess.run(
            [programs["unshare"], "--user", "--map-current-user", "--net", "--keep-caps",
             "/bin/sh", "-c", setup, "isolated-dual", programs["ip"], programs["setpriv"],
             "--no-new-privs", "--inh-caps=-all", "--ambient-caps=-all", "--",
             sys.executable, "-m", module, *original,
             "--host-probe-port", str(listener.getsockname()[1])],
            env=env, stdin=subprocess.DEVNULL,
        )
    return completed.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--dependencies", type=Path, required=True)
    parser.add_argument("--postgres-prefix", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--profile", choices=("metrics", "full"), default="metrics")
    parser.add_argument("--with-alertmanager", action="store_true")
    parser.add_argument("--gateway-envoy", type=Path, help="Existing Envoy executable for the dual-upstream test")
    parser.add_argument("--host-probe-port", type=int, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not sys.platform.startswith("linux") or os.geteuid() == 0:
        parser.error("Use an unprivileged Linux account")
    args.work_dir = args.work_dir.resolve()
    os.umask(0o077)
    try:
        if args.host_probe_port is not None:
            exercise(args)
            return 0
        return enter_namespace(args, sys.argv[1:])
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Isolated dual-instance test: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
