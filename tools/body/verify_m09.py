"""Run sequential Linux actuator timing rounds and preserve the raw cycle data."""

import argparse
from collections import Counter
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import selectors
import subprocess
import sys
import time


PERIOD = 10_000_000
SECOND = 1_000_000_000
WORK = """
from datetime import timedelta
import json, sys, time
begin, end = map(int, sys.argv[1:])
time.sleep(max(0.0, (begin - time.monotonic_ns()) / 1e9))
cpu = time.process_time_ns()
actual = time.monotonic_ns()
n = 1
while time.monotonic_ns() < end:
    n = (n * 16807) % 2147483647
print(json.dumps({"startNs": actual, "finishNs": time.monotonic_ns(),
                  "cpuMs": (time.process_time_ns() - cpu) / 1e6, "checksum": n}))
"""


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def write_json(path, body):
    path.write_text(json.dumps(body, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def sample_stats(samples):
    if not samples:
        return None
    samples.sort()
    return {
        key: round(samples[math.ceil(q * len(samples)) - 1] / 1000, 3)
        for key, q in (("p50", .50), ("p95", .95), ("p99", .99), ("max", 1))
    }


def window(rows):
    timings = {key.removesuffix("Ns") + "Us": sample_stats([row[key] for row in rows])
               for key in ("wakeLateNs", "executeNs", "finishNs", "readNs", "updateNs", "writeNs")}
    timings["intervalErrorUs"] = sample_stats([
        abs(row["intervalNs"] - PERIOD) for row in rows if row["intervalNs"] > 0
    ])
    return {
        "cycles": len(rows),
        "deadlineMisses": sum(row["missed"] for row in rows),
        "skippedSlots": sum(row["skipped"] for row in rows),
        "outputReasons": dict(Counter(row["reason"] for row in rows)),
        "timing": timings,
    }


def round_report(directory, pin, probe, seconds, number):
    csv_path = directory / f"round-{number}.csv"
    started = utc_now()
    print(f"round {number} started at {started}", flush=True)
    load_before = os.getloadavg()
    control = subprocess.Popen([*pin, str(probe), str(seconds), str(csv_path)],
                               stdout=subprocess.PIPE, text=True)
    worker = None
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(control.stdout, selectors.EVENT_READ)
            if not selector.select(5):
                raise RuntimeError("probe did not publish its start marker")
            ready = control.stdout.readline().split()
        if len(ready) != 2 or ready[0] != "ready":
            raise RuntimeError("invalid probe marker: " + " ".join(ready))
        origin = int(ready[1])
        midpoint = origin + seconds * SECOND // 2
        end = origin + seconds * SECOND
        worker = subprocess.Popen([*pin, sys.executable, "-c", WORK, str(midpoint), str(end)],
                                  stdout=subprocess.PIPE, text=True)
        output, _ = control.communicate(timeout=seconds + 25)
        worker_output, _ = worker.communicate(timeout=4)
        if control.returncode or worker.returncode:
            raise RuntimeError(f"probe {control.returncode}, load worker {worker.returncode}")
        disturbance = json.loads(worker_output)
    finally:
        stop(control)
        stop(worker)
    with csv_path.open(encoding="utf-8", newline="") as source:
        rows = [{field: value if field == "reason" else int(value) for field, value in row.items()}
                for row in csv.DictReader(source)]
    if output.strip() != f"cycles {len(rows)}" or len(rows) + sum(r["skipped"] for r in rows) != seconds * 100:
        raise RuntimeError("release counts or CSV length do not match the fixed schedule")
    if abs(disturbance["startNs"] - midpoint) > SECOND or abs(disturbance["finishNs"] - end) > SECOND:
        raise RuntimeError("load process did not align with the planned phase")
    guard = min(2 * SECOND, seconds * SECOND // 20)
    split = disturbance["startNs"] - origin
    result = {
        "round": number, "startedUtc": started, "completedUtc": utc_now(), "seconds": seconds,
        "loadAverageBefore": load_before, "loadAverageAfter": os.getloadavg(),
        "cpuLoad": {
            "startOffsetMs": split / 1e6, "finishOffsetMs": (disturbance["finishNs"] - origin) / 1e6,
            "cpuMs": disturbance["cpuMs"], "transitionGuardMs": guard / 1e6,
        },
        "rawFile": csv_path.name,
        "rawSha256": hashlib.sha256(csv_path.read_bytes()).hexdigest(),
        "all": window(rows),
        "ambient": window([r for r in rows if r["releaseNs"] < split - guard]),
        "sameCpuLoad": window([r for r in rows if r["releaseNs"] >= split + guard]),
    }
    write_json(directory / f"round-{number}.json", result)
    print(f'round {number} finished: {result["all"]["cycles"]} cycles, '
          f'{result["all"]["skippedSlots"]} skipped', flush=True)
    return result


def sys_value(path):
    try:
        return Path(path).read_text(encoding="utf-8").strip()
    except FileNotFoundError:
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--report-dir", required=True, type=Path)
    parser.add_argument("--cpu", required=True, type=int)
    parser.add_argument("--seconds", default=1800, type=int)
    parser.add_argument("--rounds", default=3, type=int)
    args = parser.parse_args()
    if sys.platform != "linux" or not 4 <= args.seconds <= 1800 or not 1 <= args.rounds <= 10:
        parser.error("Linux, 4–1800 seconds and 1–10 sequential rounds required")
    if args.cpu not in os.sched_getaffinity(0) or os.sched_getscheduler(0) != os.SCHED_OTHER:
        parser.error("choose a CPU in the current affinity set and use the normal scheduler")
    probe = args.probe.resolve(strict=True)
    args.report_dir.mkdir()
    pin = ["taskset", "-c", str(args.cpu)]
    root = Path(__file__).resolve().parents[2]
    sources = [(path.relative_to(root).as_posix(), hashlib.sha256(path.read_bytes()).hexdigest())
               for path in sorted((root / "robotics/actuator_control").rglob("*")) if path.is_file()]
    evidence = {
        "kind": "linux-actuator-soft-timing", "periodNs": PERIOD, "secondsPerRound": args.seconds,
        "expectedRounds": args.rounds, "cpu": args.cpu, "scheduler": "SCHED_OTHER",
        "visibleCpus": sorted(os.sched_getaffinity(0)), "kernelRelease": os.uname().release,
        "kernelBuild": os.uname().version, "realtime": sys_value("/sys/kernel/realtime"),
        "isolatedCpuList": sys_value("/sys/devices/system/cpu/isolated"),
        "clockResolutionNs": round(time.get_clock_info("monotonic").resolution * SECOND),
        "probeSha256": hashlib.sha256(probe.read_bytes()).hexdigest(),
        "sourceSha256": dict(sources), "rounds": [],
    }
    report = args.report_dir / "summary.json"
    write_json(report, evidence)
    for number in range(1, args.rounds + 1):
        evidence["rounds"].append(round_report(args.report_dir, pin, probe, args.seconds, number))
        write_json(report, evidence)
    print(report)


if __name__ == "__main__":
    main()
