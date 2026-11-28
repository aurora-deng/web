"""Use original scrape timestamps to calculate instance-scoped service objectives."""

from collections import defaultdict
import json
import math

from tools.observability.observer.metrics import service_levels
from tools.observability.observer.sources.sources import BackendError, HTTP_RESPONSES


INTERVAL = 15
EPOCH = "nebula_process_start_time_seconds"
IDENTITY_LABELS = ("route", "stage", "le", "status_class")
METRICS = ("up", EPOCH, HTTP_RESPONSES, *service_levels.TRACE_METRICS, *service_levels.HTTP_TRACE_METRICS,
           "nebula_http_handler_seconds_count", "nebula_http_handler_seconds_bucket",
           "nebula_chat_stage_seconds_count", "nebula_chat_stage_seconds_bucket")


def samples(matrix, instance):
    by_time = defaultdict(dict)
    for series in matrix:
        labels = series["metric"]
        name = labels.get("__name__")
        if name not in METRICS or labels.get("instance") != instance or labels.get("job") != "nebula":
            continue
        key = name, tuple((label, labels[label]) for label in sorted(IDENTITY_LABELS) if label in labels)
        for stamp, raw in series["values"]:
            bucket = by_time[int(stamp)]
            value = float(raw)
            if key in bucket and bucket[key] != value:
                raise BackendError("同一实例出现重复的指标序列，请检查采集标签")
            bucket[key] = value
    rows = []
    for stamp, values in sorted(by_time.items()):
        if ("up", ()) not in values:
            continue
        scraped = values["up", ()] == 1
        epoch = values.get((EPOCH, ()), 0)
        rows.append((stamp, {"_instance": instance, "_reliability": {
            "epoch": epoch, "scrape": scraped, "persisted": True,
            "series": service_levels.capture_counters(values) if scraped and math.isfinite(epoch) and epoch > 0 else {},
        }}))
    return rows


def report(sources, start, end, requested):
    discovered = sources.prom("query", {"query": 'up{job="nebula"}', "time": end})
    instances = sorted({row["metric"]["instance"] for row in discovered.get("result", []) if row["metric"].get("instance")})
    if (len(instances) > 1 and not requested) or (requested and requested not in instances):
        return {"source": "prometheus", "status": "choose_instance", "instances": instances, "from": start, "to": end}
    instance = requested or (instances[0] if instances else None)
    rows = []
    if instance:
        # Service-level data always includes the rolling budget window. Prometheus returns
        # true sample timestamps for a raw range selector, including a failed up sample.
        seconds = service_levels.PERIOD + 2 * INTERVAL
        expression = ('{job="nebula",instance=' + json.dumps(instance) + ',__name__=~"' + '|'.join(METRICS) +
                      '",le=~"0[.]04|0[.]08|",route=~"conversations|",stage=~"ack_ready|"}' + f'[{seconds}s]')
        response = sources.prom("query", {"query": expression, "time": end})
        if response.get("resultType") != "matrix":
            raise BackendError("服务目标需要 Prometheus 原始范围样本")
        try:
            rows = samples(response["result"], instance)
        except (KeyError, IndexError, ValueError, OverflowError, TypeError) as error:
            raise BackendError("Prometheus 返回了无效的计数样本") from error
    return {**service_levels.report(rows, start, end, INTERVAL),
            "source": "prometheus", "instance": instance, "instances": instances}
