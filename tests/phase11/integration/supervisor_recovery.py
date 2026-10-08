#!/usr/bin/env python3

"""Isolated crash/restart test for the user-space Phase 11 supervisor."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time


def wait_until(predicate, timeout: float, description: str):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {description}")


def read_live_pid(path: Path) -> int | None:
    try:
        pid = int(path.read_text().strip())
        os.kill(pid, 0)
        # kill(pid, 0) also succeeds for a zombie.  A zombie has already exited
        # and must be treated as dead by both the test and the supervisor.
        fields = Path(f"/proc/{pid}/stat").read_text().split()
        if len(fields) < 22 or fields[2] in ("Z", "X"):
            return None
        return pid
    except (FileNotFoundError, ProcessLookupError, PermissionError, ValueError):
        return None


def verify_restart_budget(supervisor: Path) -> None:
    """A permanently failing service must stop after the bounded budget."""
    with tempfile.TemporaryDirectory(prefix="phase11-supervisor-budget-") as temp:
        root = Path(temp)
        failing_service = root / "failing-service.sh"
        failing_service.write_text(
            """#!/usr/bin/env bash
set -euo pipefail
case "${1:-status}" in
  start) exit 1 ;;
  stop) exit 0 ;;
  *) exit 2 ;;
esac
""",
            encoding="utf-8",
        )
        failing_service.chmod(0o700)
        environment = os.environ.copy()
        environment.update(
            {
                "PHASE11_DATA_ROOT": str(root),
                "PHASE11_SERVICE_SCRIPT": str(failing_service),
                "PHASE11_SUPERVISOR_HEALTH_URL": "-",
                "PHASE11_SUPERVISOR_RESTART_LIMIT": "2",
                "PHASE11_SUPERVISOR_RESTART_WINDOW_SECONDS": "30",
                "PHASE11_SUPERVISOR_STABLE_SECONDS": "30",
                "PHASE11_SUPERVISOR_INITIAL_BACKOFF_SECONDS": "0.01",
                "PHASE11_SUPERVISOR_MAX_BACKOFF_SECONDS": "0.02",
            }
        )
        completed = subprocess.run(
            ["bash", str(supervisor), "run"],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=5,
            check=False,
        )
        if completed.returncode == 0:
            raise AssertionError("permanent start failure bypassed restart budget")
        events = (root / "operations.jsonl").read_text(encoding="utf-8")
        if events.count('"event":"start_failed"') != 3:
            raise AssertionError("restart budget did not count all failed starts")
        if '"event":"restart_budget_exhausted"' not in events:
            raise AssertionError("restart budget exhaustion was not audited")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--supervisor", type=Path, required=True)
    args = parser.parse_args()
    supervisor = args.supervisor.resolve(strict=True)

    with tempfile.TemporaryDirectory(prefix="phase11-supervisor-") as temp:
        root = Path(temp)
        fake_service = root / "fake-service.sh"
        fake_service.write_text(
            """#!/usr/bin/env bash
set -euo pipefail
data_root="${PHASE11_DATA_ROOT:?}"
pid_file="${data_root}/webserver.pid"
ticks_file="${pid_file}.start_ticks"
case "${1:-status}" in
  start)
    if [[ -r "${pid_file}" ]]; then
      read -r pid < "${pid_file}"
      state="$(awk '{print $3}' "/proc/${pid}/stat" 2>/dev/null || true)"
      if kill -0 "${pid}" 2>/dev/null && [[ "${state}" != Z && "${state}" != X ]]; then
        exit 0
      fi
    fi
    nohup sleep 300 >/dev/null 2>&1 &
    pid=$!
    printf '%s\\n' "${pid}" > "${pid_file}"
    awk '{print $22}' "/proc/${pid}/stat" > "${ticks_file}"
    ;;
  stop)
    if [[ -r "${pid_file}" ]]; then
      read -r pid < "${pid_file}"
      kill -TERM "${pid}" 2>/dev/null || true
      for _ in {1..50}; do
        kill -0 "${pid}" 2>/dev/null || break
        sleep 0.02
      done
    fi
    rm -f "${pid_file}" "${ticks_file}"
    ;;
  status) test -r "${pid_file}" ;;
  *) exit 2 ;;
esac
""",
            encoding="utf-8",
        )
        fake_service.chmod(0o700)
        output = root / "supervisor-output.log"
        environment = os.environ.copy()
        environment.update(
            {
                "PHASE11_DATA_ROOT": str(root),
                "PHASE11_SERVICE_SCRIPT": str(fake_service),
                "PHASE11_SUPERVISOR_HEALTH_URL": "-",
                "PHASE11_SUPERVISOR_MONITOR_SECONDS": "0.05",
                "PHASE11_SUPERVISOR_HEALTH_FAILURES": "2",
                "PHASE11_SUPERVISOR_RESTART_LIMIT": "3",
                "PHASE11_SUPERVISOR_RESTART_WINDOW_SECONDS": "10",
                "PHASE11_SUPERVISOR_STABLE_SECONDS": "2",
                "PHASE11_SUPERVISOR_INITIAL_BACKOFF_SECONDS": "0.05",
                "PHASE11_SUPERVISOR_MAX_BACKOFF_SECONDS": "0.1",
            }
        )
        process: subprocess.Popen[bytes] | None = None
        managed_pid_file = root / "webserver.pid"
        try:
            with output.open("wb") as log:
                process = subprocess.Popen(
                    ["bash", str(supervisor), "run"],
                    env=environment,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                )
                first_pid = wait_until(
                    lambda: read_live_pid(managed_pid_file),
                    5,
                    "initial managed process",
                )
                # Wait until the supervisor has captured PID + start ticks.
                # Killing earlier would exercise the separate startup-race path
                # (`identity_missing`) rather than monitored crash recovery.
                wait_until(
                    lambda: (
                        (root / "operations.jsonl").is_file()
                        and '"event":"service_observed"'
                        in (root / "operations.jsonl").read_text(
                            encoding="utf-8"
                        )
                    ),
                    5,
                    "supervisor observation of initial process",
                )
                os.kill(first_pid, signal.SIGKILL)

                second_pid = wait_until(
                    lambda: (
                        pid
                        if (pid := read_live_pid(managed_pid_file))
                        and pid != first_pid
                        else None
                    ),
                    5,
                    "replacement managed process",
                )
                if second_pid == first_pid:
                    raise AssertionError(
                        "supervisor did not replace the crashed process"
                    )

                process.send_signal(signal.SIGTERM)
                process.wait(timeout=5)
                if process.returncode != 0:
                    raise AssertionError(
                        f"supervisor exited with {process.returncode}: "
                        + output.read_text(errors="replace")
                    )
                if read_live_pid(managed_pid_file) is not None:
                    raise AssertionError(
                        "managed process survived supervisor shutdown"
                    )
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
            leftover = read_live_pid(managed_pid_file)
            if leftover is not None:
                os.kill(leftover, signal.SIGTERM)

        events = (root / "operations.jsonl").read_text(encoding="utf-8")
        for required in ("crash_detected", "restart_scheduled", "supervisor_stopped"):
            if f'"event":"{required}"' not in events:
                raise AssertionError(f"missing supervisor event: {required}")
        print(
            "phase11 supervisor recovery: passed "
            f"(old_pid={first_pid}, new_pid={second_pid})"
        )
    verify_restart_budget(supervisor)
    print("phase11 supervisor restart budget: passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
