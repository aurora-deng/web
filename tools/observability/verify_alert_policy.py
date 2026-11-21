"""Run the Prometheus rule evaluator with small, versioned alert fixtures."""

import argparse
import json
from pathlib import Path
import subprocess

from .observer.alert_policy import prometheus_alerts


def fixtures():
    rules = prometheus_alerts()
    definition = {"groups": [{"name": "nebula", "rules": rules}]}
    by_name = {row["alert"]: row for row in rules}
    scope = {"bc": "lab", "app": "nebula", "instance": "nebula-a"}

    def series(metric, values, **labels):
        attached = {**scope, "job": "nebula", **labels}
        selectors = ",".join(f'{key}="{value}"' for key, value in attached.items())
        return {"series": f"{metric}{{{selectors}}}", "values": values}

    def check(alert, stamp, active, keep_job=False):
        spec = by_name[alert]
        labels = {**scope, **spec["labels"], **({"job": "nebula"} if keep_job else {})}
        return {"eval_time": stamp, "alertname": alert,
                "exp_alerts": [{"exp_labels": labels, "exp_annotations": spec["annotations"]}] if active else []}

    def http(good, bad):
        return [series("nebula_business_http_responses_total", good, status_class="2xx"),
                series("nebula_business_http_responses_total", bad, status_class="5xx")]

    def hist(count):
        base = "nebula_database_operation_seconds"
        return [series(base+"_count", count), series(base+"_bucket", "0x40", le="0.25"),
                series(base+"_bucket", count, le="0.5"), series(base+"_bucket", count, le="+Inf")]

    cases = [
        (http("0x40", "0+1x40"), [check("NebulaHttpHighErrors", "7m", False)]),
        (http("0+17x40", "0+3x40"), [check("NebulaHttpHighErrors", "7m", True)]),
        (hist("0+1x40"), [check("NebulaDatabaseSlowP99", "8m", False)]),
        (hist("0+15x40"), [check("NebulaDatabaseSlowP99", "8m", True)]),
        ([series("nebula_realtime_pending_age_seconds", "20x12 0x14")],
         [check("NebulaRealtimePendingOld", "2m", True, True), check("NebulaRealtimePendingOld", "4m", False)]),
        ([series("nebula_telemetry_dropped_total", "0x5 5x40")],
         [check("NebulaTelemetryDropped", "4m", True, True), check("NebulaTelemetryDropped", "8m", False)]),
    ]
    test = {"rule_files": ["alerts.yaml"], "evaluation_interval": "15s",
            "tests": [{"interval": "15s", "input_series": input_series, "alert_rule_test": expected}
                      for input_series, expected in cases]}
    return {"alerts.yaml": definition, "policy-tests.yaml": test}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--promtool", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    for name, content in fixtures().items():
        (args.output/name).write_text(json.dumps(content, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    subprocess.run([str(args.promtool.resolve()), "test", "rules", "policy-tests.yaml"], cwd=args.output,
                   check=True, timeout=30)


if __name__ == "__main__":
    main()
