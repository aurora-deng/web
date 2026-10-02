#!/usr/bin/env python3
"""Compare two five-run Phase 10 benchmark summaries against acceptance gates."""

import json
import pathlib
import sys


def load(path: str) -> dict:
    return json.loads(pathlib.Path(path).read_text())


def gain(before: float, after: float) -> float:
    return (after / before - 1.0) * 100.0


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: compare_phase10_results.py BASELINE_SUMMARY CURRENT_SUMMARY")
        return 1
    before, after = map(load, sys.argv[1:])
    checks = []

    http_gain = gain(before["http_qps_median"], after["http_qps_median"])
    checks.append((http_gain >= 20.0, f"HTTP QPS gain {http_gain:.2f}% (need >=20%)"))
    checks.append((after["http_errors"] == 0,
                   f"HTTP errors {after['http_errors']} (need 0)"))
    checks.append((after["http_p99_us_median"] <=
                   before["http_p99_us_median"] * 1.10,
                   "HTTP P99 must not regress by more than 10%"))

    grpc_gain = gain(before["grpc_qps_median"]["echo"],
                     after["grpc_qps_median"]["echo"])
    checks.append((grpc_gain >= 15.0, f"gRPC Echo QPS gain {grpc_gain:.2f}% (need >=15%)"))
    checks.append((after["grpc_errors"] == 0,
                   f"gRPC errors {after['grpc_errors']} (need 0)"))
    checks.append((after["grpc_p99_us_median"]["echo"] <=
                   before["grpc_p99_us_median"]["echo"] * 1.10,
                   "gRPC Echo P99 must not regress by more than 10%"))

    for passed, description in checks:
        print(("PASS " if passed else "FAIL ") + description)
    return 0 if all(passed for passed, _ in checks) else 2


if __name__ == "__main__":
    raise SystemExit(main())
