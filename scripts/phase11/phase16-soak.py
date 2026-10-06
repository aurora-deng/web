#!/usr/bin/env python3
"""Bounded liveness/readiness/metrics soak probe using only stdlib."""

from __future__ import annotations

import argparse
import json
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path


def fetch(url: str, timeout: float) -> tuple[int, bytes]:
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return response.status, response.read()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:18090")
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.duration <= 0 or args.interval <= 0 or args.timeout <= 0:
        parser.error("duration, interval and timeout must be positive")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + args.duration
    samples = failures = 0
    with args.output.open("w", encoding="utf-8") as output:
        while time.monotonic() < deadline:
            started = time.monotonic()
            record: dict[str, object] = {
                "time": datetime.now(timezone.utc).isoformat(),
                "ok": False,
            }
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
                    ok=live_status == ready_status == metrics_status == 200,
                )
            except (OSError, urllib.error.URLError, TimeoutError) as error:
                record["error"] = str(error)
            record["latencyMs"] = round((time.monotonic() - started) * 1000, 3)
            samples += 1
            if not record["ok"]:
                failures += 1
            output.write(json.dumps(record, ensure_ascii=False) + "\n")
            output.flush()
            time.sleep(max(0.0, args.interval - (time.monotonic() - started)))

    print(json.dumps({"samples": samples, "failures": failures, "output": str(args.output)}))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
