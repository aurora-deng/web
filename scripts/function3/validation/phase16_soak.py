#!/usr/bin/env python3
"""Bounded service soak probe with process and resource-leak evidence.

Only the Python standard library is used. A long run pins the expected server
PID, probes liveness/readiness/metrics, samples Linux /proc resource counters,
and writes an atomic summary whose SHA-256 identifies the raw JSONL evidence.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def fetch(url: str, timeout: float) -> tuple[int, bytes]:
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return response.status, response.read()


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, math.ceil(fraction * len(ordered)) - 1))
    return round(ordered[index], 3)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def process_snapshot(pid: int) -> dict[str, int]:
    """Read stable, low-cost Linux process counters without psutil."""
    root = Path("/proc") / str(pid)
    status = (root / "status").read_text(encoding="utf-8")
    stat = (root / "stat").read_text(encoding="utf-8")
    _, separator, stat_tail = stat.rpartition(")")
    stat_fields = stat_tail.strip().split()
    if not separator or len(stat_fields) <= 19 or not stat_fields[19].isdigit():
        raise RuntimeError("process start time is absent from /proc stat")
    fields: dict[str, int] = {}
    for line in status.splitlines():
        name, separator, value = line.partition(":")
        if not separator or name not in {"VmRSS", "VmSize", "Threads"}:
            continue
        parts = value.strip().split(maxsplit=1)
        if parts and parts[0].isdigit():
            first = parts[0]
            fields[name] = int(first)
    fields["fdCount"] = sum(1 for _ in (root / "fd").iterdir())
    if "VmRSS" not in fields or "Threads" not in fields:
        raise RuntimeError("required /proc counters are absent")
    return {
        "rssKiB": fields["VmRSS"],
        "virtualKiB": fields.get("VmSize", 0),
        "threads": fields["Threads"],
        "fdCount": fields["fdCount"],
        # Linux clock ticks since boot. PID 相同但该值变化，说明原进程已被替换。
        "startTicks": int(stat_fields[19]),
    }


def write_json_atomic(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    os.replace(temporary, path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:18090")
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path)
    parser.add_argument("--expected-pid", type=int)
    parser.add_argument(
        "--max-rss-growth-kib",
        type=int,
        default=0,
        help="fail when final minus initial RSS exceeds this limit; 0 disables the gate",
    )
    args = parser.parse_args()
    if args.duration <= 0 or args.interval <= 0 or args.timeout <= 0:
        parser.error("duration, interval and timeout must be positive")
    if args.expected_pid is not None and args.expected_pid <= 0:
        parser.error("expected PID must be positive")
    if args.max_rss_growth_kib < 0:
        parser.error("RSS growth limit cannot be negative")

    summary_path = args.summary or args.output.with_suffix(args.output.suffix + ".summary.json")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists() or summary_path.exists():
        parser.error("output and summary paths must not already exist")

    started_at = utc_now()
    started = time.monotonic()
    deadline = started + args.duration
    samples = failures = 0
    latencies: list[float] = []
    resources: list[dict[str, int]] = []
    expected_start_ticks: int | None = None

    with args.output.open("x", encoding="utf-8") as output:
        while time.monotonic() < deadline:
            sample_started = time.monotonic()
            record: dict[str, Any] = {"time": utc_now(), "ok": False}
            try:
                live_status, live_body = fetch(f"{args.base_url}/health/live", args.timeout)
                ready_status, ready_body = fetch(f"{args.base_url}/health/ready", args.timeout)
                metrics_status, metrics_body = fetch(f"{args.base_url}/metrics", args.timeout)
                record.update(
                    liveStatus=live_status,
                    readyStatus=ready_status,
                    metricsStatus=metrics_status,
                    liveBody=live_body.decode("utf-8", "replace"),
                    readyBody=ready_body.decode("utf-8", "replace"),
                    metricsBytes=len(metrics_body),
                )
                record["ok"] = live_status == ready_status == metrics_status == 200
                if args.expected_pid is not None:
                    snapshot = process_snapshot(args.expected_pid)
                    if expected_start_ticks is None:
                        expected_start_ticks = snapshot["startTicks"]
                    elif snapshot["startTicks"] != expected_start_ticks:
                        raise RuntimeError("expected PID was reused by another process")
                    record["process"] = snapshot
                    resources.append(snapshot)
            except (OSError, RuntimeError, urllib.error.URLError, TimeoutError) as error:
                # HTTP 探测成功但固定进程已经退出、被替换或无法采样时，整次采样仍应失败。
                # 否则长稳测试会把“端口被其他进程接管”误报为原服务仍然健康。
                record["ok"] = False
                record["error"] = str(error)
            latency = round((time.monotonic() - sample_started) * 1000, 3)
            record["latencyMs"] = latency
            latencies.append(latency)
            samples += 1
            if not record["ok"]:
                failures += 1
            output.write(json.dumps(record, ensure_ascii=False) + "\n")
            output.flush()
            time.sleep(max(0.0, args.interval - (time.monotonic() - sample_started)))

    rss_start = resources[0]["rssKiB"] if resources else None
    rss_end = resources[-1]["rssKiB"] if resources else None
    rss_growth = rss_end - rss_start if rss_start is not None and rss_end is not None else None
    rss_gate_failed = bool(
        args.max_rss_growth_kib
        and rss_growth is not None
        and rss_growth > args.max_rss_growth_kib
    )
    summary: dict[str, Any] = {
        "schema": "phase11-soak-summary-v2",
        "startedAt": started_at,
        "completedAt": utc_now(),
        "requestedDurationSeconds": args.duration,
        "actualDurationSeconds": round(time.monotonic() - started, 3),
        "intervalSeconds": args.interval,
        "samples": samples,
        "failures": failures,
        "expectedPid": args.expected_pid,
        "expectedStartTicks": expected_start_ticks,
        "latencyMs": {
            "p50": percentile(latencies, 0.50),
            "p95": percentile(latencies, 0.95),
            "p99": percentile(latencies, 0.99),
            "max": round(max(latencies, default=0.0), 3),
        },
        "resources": {
            "rssStartKiB": rss_start,
            "rssEndKiB": rss_end,
            "rssMaxKiB": max((item["rssKiB"] for item in resources), default=None),
            "rssGrowthKiB": rss_growth,
            "fdStart": resources[0]["fdCount"] if resources else None,
            "fdEnd": resources[-1]["fdCount"] if resources else None,
            "fdMax": max((item["fdCount"] for item in resources), default=None),
            "threadsStart": resources[0]["threads"] if resources else None,
            "threadsEnd": resources[-1]["threads"] if resources else None,
            "threadsMax": max((item["threads"] for item in resources), default=None),
        },
        "maxRssGrowthKiB": args.max_rss_growth_kib,
        "rssGateFailed": rss_gate_failed,
        "evidence": str(args.output.resolve()),
        "evidenceSha256": sha256(args.output),
    }
    write_json_atomic(summary_path, summary)
    print(json.dumps(summary, ensure_ascii=False))
    return 0 if failures == 0 and not rss_gate_failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
