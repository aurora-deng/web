#!/usr/bin/env python3

"""聚合多轮 Phase 15 业务基准，使用中位数降低单轮抖动。"""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path


def median(values: list[float]) -> float | None:
    return round(statistics.median(values), 3) if values else None


def main() -> int:
    parser = argparse.ArgumentParser(description="Aggregate Phase 15 benchmark runs")
    parser.add_argument("reports", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    reports = [json.loads(path.read_text(encoding="utf-8"))
               for path in args.reports]
    if any(item.get("schema") != "phase15-business-benchmark-v1"
           for item in reports):
        parser.error("all inputs must use phase15-business-benchmark-v1")

    grouped: dict[str, list[dict]] = {}
    for report in reports:
        for result in report["results"]:
            grouped.setdefault(result["name"], []).append(result)

    scenarios = []
    total_errors = 0
    for name, runs in grouped.items():
        errors = sum(int(item["errors"]) for item in runs)
        total_errors += errors
        scenario = {
            "name": name,
            "runs": len(runs),
            "operationsPerSecondMedian": median(
                [float(item["operationsPerSecond"]) for item in runs]),
            "unitsPerSecondMedian": median(
                [float(item["unitsPerSecond"]) for item in runs]),
            "latencyMsMedian": {
                key: median([float(item["latencyMs"][key]) for item in runs])
                for key in ("average", "p50", "p95", "p99", "maximum")
            },
            "errors": errors,
        }
        accepted = [item["databaseAcceptedLatencyMs"] for item in runs
                    if "databaseAcceptedLatencyMs" in item]
        if accepted:
            scenario["databaseAcceptedLatencyMsMedian"] = {
                key: median([float(item[key]) for item in accepted])
                for key in ("p50", "p95", "p99")
            }
        scenarios.append(scenario)

    rss_peaks = [float(report["resources"]["rssKiB"]["peak"])
                 for report in reports if report["resources"].get("available")]
    fd_peaks = [float(report["resources"]["fileDescriptors"]["peak"])
                for report in reports if report["resources"].get("available")]
    thread_peaks = [float(report["resources"]["threads"]["peak"])
                    for report in reports if report["resources"].get("available")]
    cpu_averages = [float(report["resources"]["cpuPercent"]["average"])
                    for report in reports
                    if report["resources"].get("available") and
                    report["resources"]["cpuPercent"].get("average") is not None]
    cpu_peaks = [float(report["resources"]["cpuPercent"]["peak"])
                 for report in reports
                 if report["resources"].get("available") and
                 report["resources"]["cpuPercent"].get("peak") is not None]
    summary = {
        "schema": "phase15-business-benchmark-summary-v1",
        "runCount": len(reports),
        "configuration": reports[0]["configuration"],
        "target": reports[0]["target"],
        "elapsedSecondsMedian": median(
            [float(report["elapsedSeconds"]) for report in reports]),
        "totalErrors": total_errors,
        "scenarios": scenarios,
        "resourcePeakMedian": {
            "rssKiB": median(rss_peaks),
            "fileDescriptors": median(fd_peaks),
            "threads": median(thread_peaks),
            "cpuAveragePercent": median(cpu_averages),
            "cpuPeakPercent": median(cpu_peaks),
        },
        "sourceReports": [str(path) for path in args.reports],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps(summary, ensure_ascii=False, indent=2) + "\n"
    args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0 if total_errors == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
