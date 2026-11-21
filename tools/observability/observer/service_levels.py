"""Request-based service indicators over a continuous segment of local scrapes."""
from bisect import bisect_left
from dataclasses import dataclass

from .sources import HTTP_RESPONSES

PERIOD = 6 * 3600
MIN_EVENTS = 20
TRACE_METRICS = ("nebula_telemetry_exported_total", "nebula_telemetry_dropped_total",
                 "nebula_telemetry_queue_full_total")
HTTP_TRACE_METRICS = ("nebula_http_trace_sampling_skipped_total", "nebula_http_trace_failure_roots_total")


@dataclass(frozen=True)
class Objective:
    key: str
    title: str
    target: float
    description: str
    panel: str
    operation: str = ""
    latency_ms: int = 0


OBJECTIVES = (
    Objective("adapter_http", "业务 HTTP 非 5xx 比例", 0.99,
              "统计共享业务适配器完成的 HTTP 响应，客户端错误也计入非 5xx。未覆盖全站和客户端超时。",
              "http_5xx_pct"),
    Objective("conversation_latency", "会话列表在 80 ms 内处理", 0.99,
              "从业务 handler 开始到响应就绪，使用 ≤80 ms 的延迟桶。包含已计时的错误响应。",
              "http_p99", "GET /api/conversations", 80),
    Objective("chat_ack", "聊天 ACK 在 40 ms 内就绪", 0.99,
              "统计 WebSocket 聊天到达 ACK 就绪的样本，包含幂等重试。客户端往返和未到达 ACK 的请求另测。",
              "chat_ack_ready", "WS chat.send", 40),
)
# The alerting windows are sized for the six-hour learning budget.
BURN_WINDOWS = (("fast", 60, 300, 0.05), ("slow", 300, 1800, 0.15))


def capture_counters(metrics):
    def raw(name, **labels):
        value = metrics.get((name, tuple(sorted(labels.items()))))
        return int(value) if value is not None and 0 <= value <= 2**53 - 1 and value == int(value) else None

    def latency(name, upper, **labels):
        return (raw(name + "_bucket", le=upper, **labels), raw(name + "_count", **labels))

    groups = {
        "adapter_http": tuple(raw(HTTP_RESPONSES, status_class=f"{i}xx") for i in range(1, 6)),
        "conversation_latency": latency("nebula_http_handler_seconds", "0.08", route="conversations"),
        "chat_ack": latency("nebula_chat_stage_seconds", "0.04", stage="ack_ready"),
        "trace": tuple(raw(metric) for metric in TRACE_METRICS),
        "http_trace_sampling": tuple(raw(metric) for metric in HTTP_TRACE_METRICS),
    }
    return {name: values for name, values in groups.items() if None not in values}


def counter_window(rows, key, start, end, interval):
    """Take counter differences only inside one continuously observed epoch."""
    result = {"from": start, "to": end, "firstSampleAt": None, "lastSampleAt": None,
              "samples": 0, "coverageSeconds": 0, "increments": None, "state": "no_data"}
    if not rows:
        return {**result, "reason": "no_samples"}
    last, newest = rows[-1]
    head = newest.get("_reliability")
    max_gap = 2 * interval
    if last <= start or end - last > max_gap:
        return {**result, "lastSampleAt": last, "reason": "stale"}
    if not head:
        return {**result, "lastSampleAt": last, "reason": "before_capture"}
    if not head["scrape"]:
        return {**result, "lastSampleAt": last, "reason": "scrape_failed"}
    current = head["series"].get(key)
    if current is None:
        return {**result, "lastSampleAt": last, "reason": "missing_counter"}

    first, baseline, count, reason = last, current, 1, None
    index = bisect_left(rows, (start,))
    for offset in range(len(rows)-2, index-1, -1):
        stamp, values = rows[offset]
        previous = values.get("_reliability")
        if first - stamp > max_gap:
            reason = "gap"
        elif not previous:
            reason = "before_capture"
        elif (newest.get("_instance"), head["epoch"]) != (values.get("_instance"), previous["epoch"]):
            reason = "epoch_changed"
        elif not previous["scrape"]:
            reason = "scrape_failed"
        else:
            older = previous["series"].get(key)
            if older is None or len(older) != len(baseline):
                reason = "missing_counter"
            else:
                increments = [new-old for new, old in zip(baseline, older)]
                if min(increments) < 0:
                    reason = "reset"
                elif key in ("conversation_latency", "chat_ack") and increments[0] > increments[1]:
                    reason = "inconsistent_bucket"
        if reason:
            break
        first, baseline = stamp, older
        count += 1
    complete = not reason and last - first >= end - start - max_gap and count >= 2
    return {**result, "firstSampleAt": first, "lastSampleAt": last, "samples": count,
            "coverageSeconds": last-first, "state": "data" if complete else "partial",
            "reason": reason or (None if complete else "warming"),
            "increments": [new-old for new, old in zip(current, baseline)] if count >= 2 else None}


def objective_window(objective, window):
    increments = window.pop("increments")
    total = bad = None
    state = window["state"]
    if increments is not None:
        total, bad = ((sum(increments), increments[-1]) if objective.key == "adapter_http"
                      else (increments[1], increments[1]-increments[0]))
    state = ("no_traffic" if total == 0 else "low_volume" if total < MIN_EVENTS else "evaluated") if state == "data" else state
    bad_fraction = bad/total if total else None
    # Use exact basis points for the taught target instead of subtracting floats near 100%.
    budget = (10000-round(10000*objective.target))/10000
    burn = bad_fraction/budget if bad_fraction is not None else None
    return {**window, "state": state, "events": total, "bad": bad,
            "goodFraction": 1-bad_fraction if bad_fraction is not None else None, "burnRate": burn,
            "budgetRemainingFraction": 1-burn if state == "evaluated" else None}


def report(rows, start, end, interval):
    # Rows loaded from disk may overlap the first live scrape at a one-second
    # boundary. Keep the newest value for that stamp, matching the SQLite key.
    ordered = sorted(dict(rows).items())
    ordered = ordered[:bisect_left(ordered, (end+1,))]
    last, values = ordered[-1] if ordered else (None, {})
    if last is None or end - last > 2*interval or last < start or not (health := values.get("_reliability")):
        health = {}

    def window(key, first):
        return counter_window(ordered, key, first, end, interval)

    objectives = []
    for spec in OBJECTIVES:
        starts = {start, end-PERIOD}
        starts.update(end-length for _, short, long, _ in BURN_WINDOWS for length in (short, long))
        readings = {first: objective_window(spec, window(spec.key, first)) for first in starts}
        burns = []
        for name, short, long, fraction in BURN_WINDOWS:
            brief, sustained = readings[end-short], readings[end-long]
            threshold = fraction*PERIOD/long
            if brief["state"] == sustained["state"] == "evaluated":
                state = "burning" if min(brief["burnRate"], sustained["burnRate"]) >= threshold else "within"
            else:
                state = "insufficient"
            burns.append({"name": name, "threshold": threshold, "state": state, "short": brief, "long": sustained})
        objectives.append({"id": spec.key, "title": spec.title, "target": spec.target, "description": spec.description,
                           "panel": spec.panel, "operation": spec.operation, "latencyMs": spec.latency_ms,
                           "observed": readings[start], "budget": readings[end-PERIOD], "burns": burns})

    telemetry = window("trace", start)
    counts = telemetry.pop("increments")
    trace = dict(zip(("exported", "dropped", "queueFull"), counts)) if counts is not None else None
    sampling = window("http_trace_sampling", start)
    counts = sampling.pop("increments")
    http_sampling = dict(zip(("skipped", "failureRoots"), counts)) if counts is not None else None
    return {"source": "local", "from": start, "to": end, "instance": values.get("_instance"),
            "lastSampleAt": last, "periodSeconds": PERIOD, "minimumEvents": MIN_EVENTS, "objectives": objectives,
            "collecting": {key: health.get(key) for key in ("live", "ready", "scrape", "persisted")},
            "telemetry": {**telemetry, "counts": trace},
            "httpTraceSampling": {**sampling, "counts": http_sampling}}
