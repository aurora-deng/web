#!/usr/bin/env python3

"""聚合多轮 Phase 15/26 业务基准，使用中位数降低单轮抖动。"""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path


def median(values: list[float]) -> float | None:
    return round(statistics.median(values), 3) if values else None


def aggregate_reports(reports: list[dict], source_reports: list[str]) -> dict:
    """聚合已解析报告；独立函数让报告契约可以在不写临时文件时测试。"""
    if not reports:
        raise ValueError("at least one report is required")
    if any(item.get("schema") != "phase15-business-benchmark-v1"
           for item in reports):
        raise ValueError("all inputs must use phase15-business-benchmark-v1")

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
        ttft = [item["timeToFirstTokenMs"] for item in runs
                if "timeToFirstTokenMs" in item]
        if ttft:
            scenario["timeToFirstTokenMsMedian"] = {
                key: median([float(item[key]) for item in ttft])
                for key in ("p50", "p95", "p99")
            }
            scenario["tokensPerGenerationAverageMedian"] = median([
                float(item["tokensPerGenerationAverage"])
                for item in runs if "tokensPerGenerationAverage" in item
            ])
            scenario["concurrency"] = runs[0].get("concurrency")
        scale_runs = [item["connectionScale"] for item in runs
                      if "connectionScale" in item]
        if scale_runs:
            requested = sorted({
                int(point["requestedConnections"])
                for run in scale_runs for point in run
            })
            scale_summary = []
            for target in requested:
                points = [point for run in scale_runs for point in run
                          if int(point["requestedConnections"]) == target]
                summarized = {
                    "requestedConnections": target,
                    "establishedConnectionsMedian": median([
                        float(point["establishedConnections"]) for point in points
                    ]),
                    "connectionErrors": sum(
                        int(point["connectionErrors"]) for point in points),
                }
                for field in ("deltaRssKiB", "estimatedBytesPerConnection",
                              "deltaFileDescriptors",
                              "fileDescriptorsPerConnection"):
                    values = [float(point[field]) for point in points
                              if field in point]
                    if values:
                        summarized[field + "Median"] = median(values)
                scale_summary.append(summarized)
            scenario["connectionScaleMedian"] = scale_summary
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
    delta_names = sorted({
        name for report in reports
        for name in report.get("applicationMetrics", {})
                          .get("counterDeltas", {})
    })
    by_name = {scenario["name"]: scenario for scenario in scenarios}
    comparisons = {}
    baseline_chat = by_name.get("mixed-50-list-20-history-30-send")
    ai_chat = by_name.get("mixed-with-ai-chat")
    if baseline_chat and ai_chat:
        baseline_p99 = baseline_chat["latencyMsMedian"]["p99"]
        ai_p99 = ai_chat["latencyMsMedian"]["p99"]
        comparisons["ordinaryChatDuringAi"] = {
            "baselineP99Ms": baseline_p99,
            "withAiP99Ms": ai_p99,
            "p99ChangePercent": (
                round((ai_p99 / baseline_p99 - 1.0) * 100.0, 2)
                if baseline_p99 else None),
        }
    baseline_ai = by_name.get("ai-generation")
    mixed_ai = by_name.get("mixed-with-ai-generation")
    if baseline_ai and mixed_ai:
        baseline_ttft = baseline_ai["timeToFirstTokenMsMedian"]["p95"]
        mixed_ttft = mixed_ai["timeToFirstTokenMsMedian"]["p95"]
        comparisons["aiDuringOrdinaryChat"] = {
            "baselineTtftP95Ms": baseline_ttft,
            "withChatTtftP95Ms": mixed_ttft,
            "ttftP95ChangePercent": (
                round((mixed_ttft / baseline_ttft - 1.0) * 100.0, 2)
                if baseline_ttft else None),
        }
    return {
        "schema": "phase15-business-benchmark-summary-v1",
        "runCount": len(reports),
        "configuration": reports[0]["configuration"],
        "target": reports[0]["target"],
        "elapsedSecondsMedian": median(
            [float(report["elapsedSeconds"]) for report in reports]),
        "totalErrors": total_errors,
        "scenarios": scenarios,
        "comparisons": comparisons,
        "resourcePeakMedian": {
            "rssKiB": median(rss_peaks),
            "fileDescriptors": median(fd_peaks),
            "threads": median(thread_peaks),
            "cpuAveragePercent": median(cpu_averages),
            "cpuPeakPercent": median(cpu_peaks),
        },
        "applicationMetricDeltaMedian": {
            name: median([
                float(report.get("applicationMetrics", {})
                            .get("counterDeltas", {}).get(name, 0.0))
                for report in reports
            ])
            for name in delta_names
        },
        "applicationMetricFinal": reports[-1].get(
            "applicationMetrics", {}).get("after", {}),
        "sourceReports": source_reports,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Aggregate Phase 15 benchmark runs")
    parser.add_argument("reports", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    reports = [json.loads(path.read_text(encoding="utf-8"))
               for path in args.reports]
    try:
        summary = aggregate_reports(reports, [str(path) for path in args.reports])
    except ValueError as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps(summary, ensure_ascii=False, indent=2) + "\n"
    args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0 if summary["totalErrors"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
