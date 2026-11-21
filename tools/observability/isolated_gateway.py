"""Verify a one-upstream TLS/HTTP Envoy lab on a private Linux loopback network."""

import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
from tempfile import TemporaryDirectory

from .isolated_dual import enter_namespace, stop_lab, verify_loopback_isolation, wait_for_readiness


def exercise(args):
    network = verify_loopback_isolation(args.host_probe_port)
    if not (openssl := shutil.which("openssl")):
        raise RuntimeError("An existing OpenSSL binary is required for the lab certificate")
    directory = args.work_dir / "lab"
    https_port = 18445

    def interrupted(signum, _frame):
        raise SystemExit(128 + signum)

    for signum in (signal.SIGINT, signal.SIGHUP, signal.SIGTERM):
        signal.signal(signum, interrupted)
    with TemporaryDirectory(prefix="tls-", dir=args.work_dir) as tls:
        cert, key = Path(tls) / "ca.crt", Path(tls) / "key.pem"
        subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost",
                        "-keyout", key, "-out", cert], stderr=subprocess.DEVNULL, check=True, timeout=20)
        command = [sys.executable, "-m", "tools.observability.lab", "--profile", "metrics",
                   "--work-dir", directory, "--dependencies", args.dependencies, "--postgres-prefix", args.postgres_prefix,
                   "--binary", args.binary, "--gateway-envoy", args.gateway_envoy, "--gateway-https-port", str(https_port),
                   "--gateway-cert", cert, "--gateway-key", key, "--http-trace-every", "1", "--ws-trace-every", "1",
                   "--data-budget-mib", "384", "--duration", "420"]
        with (args.work_dir / "lab-console.log").open("xb") as log:
            lab = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
            try:
                wait_for_readiness(lab, directory)
                wire = subprocess.check_output([args.curl, "--http2", "--cacert", cert, "--noproxy", "127.0.0.1",
                    "--fail", "--silent", "--show-error", "--max-time", "8", "--output", "/dev/null",
                    "--write-out", "%{http_version} %{http_code}", f"https://127.0.0.1:{https_port}/health/ready"],
                    text=True, timeout=10).split()
                if wire != ["2", "200"]:
                    raise RuntimeError("Expected an actual HTTP/2 200 response with the lab certificate")
                print("GATEWAY_HTTP2_VERIFIED", flush=True)
                subprocess.run([sys.executable, "-m", "tools.observability.verify_gateway", "--run-dir", directory,
                                "--ca", cert, "--exercise-upstream-fault", "--output", directory / "exports/gateway.json"],
                               stdin=subprocess.DEVNULL, check=True, timeout=325)
            finally:
                stop_lab(lab)
    report = {"passed": True, "networkIsolation": network, "httpVersion": "2", "httpStatus": 200}
    (directory / "exports/wire.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("work-dir", "dependencies", "postgres-prefix", "binary", "gateway-envoy"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--curl", type=Path, default=Path("/usr/bin/curl"))
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
        return enter_namespace(args, sys.argv[1:], module="tools.observability.isolated_gateway")
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Isolated gateway test: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
