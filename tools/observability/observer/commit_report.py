"""Project an isolated PostgreSQL commit experiment onto the Observer report contract."""
import math
import re

from .events import Invalid

DISK_FIELDS = ("writes", "sectorsWritten", "writeMs", "ioMs",
               "weightedIoMs", "flushes", "flushMs", "inFlightAtEnd")
WAL_FIELDS = ("fsyncCount", "fsyncMilliseconds", "writeCount", "writeMilliseconds", "walBytes")
DEVICE_ID = re.compile(r"[0-9]{1,10}:[0-9]{1,10}")
DEVICE_NAME = re.compile(r"[a-zA-Z0-9_.!+-]{1,64}")
WAIT_EVENT = re.compile(r"(?:active|[a-zA-Z][a-zA-Z0-9_]{0,31}:[a-zA-Z][a-zA-Z0-9_]{0,63})")


def number(value, minimum=0, maximum=2**53 - 1):
    if type(value) not in (int, float) or not minimum <= value <= maximum or not math.isfinite(value):
        raise ValueError("invalid measurement")
    return value


def count(value, minimum=0, maximum=2**53 - 1):
    if type(value) is not int:
        raise ValueError("invalid count")
    return number(value, minimum, maximum)


def quantiles(row):
    result = {key: number(row[key]) for key in ("p50", "p95", "p99")}
    if not result["p50"] <= result["p95"] <= result["p99"]:
        raise ValueError("invalid quantile order")
    return result


def waits(values):
    if not isinstance(values, dict) or len(values) > 128:
        raise ValueError("invalid wait events")
    if any(not WAIT_EVENT.fullmatch(event) for event in values):
        raise ValueError("invalid wait event name")
    return {event: count(samples, 1) for event, samples in values.items()}


def disks(values, devices):
    if not isinstance(values, dict) or not values.keys() <= devices.keys():
        raise ValueError("unknown disk device")
    return {key: ({field: number(row[field]) for field in DISK_FIELDS if field in row}
                  | {"observedMs": number(row["observedMs"])})
            for key, row in values.items()}


def settings(values):
    version = values["server_version_num"]
    if not isinstance(version, str) or not re.fullmatch(r"[0-9]{6}", version):
        raise ValueError("invalid PostgreSQL version")
    result = {"server_version_num": count(int(version), 180000, 999999)}
    for key in ("fsync", "synchronous_commit", "track_wal_io_timing", "wal_sync_method"):
        value = values[key]
        if not isinstance(value, str) or not re.fullmatch("[a-z_]{1,32}", value):
            raise ValueError("invalid PostgreSQL setting")
        result[key] = value
    return result


def normalize_commit(report):
    try:
        binary_sha = report["binarySha256"]
        if (report["mode"] != "commit" or report["existingDatabaseUsed"] is not False
                or report["passed"] is not True or not re.fullmatch("[a-f0-9]{64}", binary_sha)):
            raise ValueError("invalid provenance")
        run = report["instrumented"]
        config, trace, observed = run["configuration"], run["trace"], run["commitObservation"]
        paired = count(observed["commitsPaired"], 1, 9900)
        covered = count(observed["commitsWithBackendSample"], 0, paired)
        slow = count(observed["slowCommits"], 0, paired)
        slow_covered = count(observed["slowCommitsWithBackendSample"], 0, min(slow, covered))
        if trace["serverUnits"] != "microseconds" or observed["clock"] != "CLOCK_MONOTONIC":
            raise ValueError("invalid clock or unit")
        if count(trace["paired"]) != paired or count(run["durableChatAckSamples"]) != paired:
            raise ValueError("ACK and COMMIT counts differ")
        device_rows = observed["diskDevices"]
        if not isinstance(device_rows, list) or len(device_rows) > 8:
            raise ValueError("invalid disk devices")
        devices = {item["device"]: item["name"] for item in device_rows}
        if len(devices) != len(device_rows) or any(not DEVICE_ID.fullmatch(key) or not DEVICE_NAME.fullmatch(name)
                                                   for key, name in devices.items()):
            raise ValueError("invalid disk device")
        window = observed["workloadMonotonicNs"]
        start = count(window["start"], maximum=2**63 - 1)
        end = count(window["end"], start + 1, 2**63 - 1)
        duration_ms = (end - start) / 1_000_000
        longest = observed["longestCommits"]
        if not isinstance(longest, list) or len(longest) != min(paired, 20):
            raise ValueError("incomplete longest commits")
        entries = []
        for row in longest:
            queue = count(row["queueUs"], -1)
            events = waits(row["backendWaitSamples"])
            polls = count(row["pollsWithinCommit"])
            if sum(events.values()) > polls:
                raise ValueError("more backend samples than polls")
            entries.append({
                "ackMs": number(row["ackMs"]),
                "queueUs": queue if queue >= 0 else None,
                "commitUs": number(row["commitUs"]),
                "commitStartOffsetMs": number(row["commitStartOffsetMs"], 0, duration_ms + 0.001),
                "pollsWithinCommit": polls,
                "backendWaitSamples": events,
                "diskDelta": disks(row["diskDelta"], devices),
            })
        wal = observed["walDelta"]
        wait_totals = waits(observed["slowCommitWaitSamples"])
        if sum(wait_totals.values()) < slow_covered or (not slow_covered and wait_totals):
            raise ValueError("inconsistent slow commit coverage")
        poll_limit = observed["sampleLimitReached"]
        if type(poll_limit) is not bool:
            raise ValueError("invalid poll limit")
        wait_polls = count(observed["waitPolls"])
        return {
            "binarySha256": binary_sha,
            "workers": count(config["workers"], 1, 64),
            "operations": count(config["operations"], paired, 9900),
            "durationMs": duration_ms,
            "controlDelayMs": count(report["commitControlDelayMs"], 0, 250),
            "ackMs": quantiles(run["durableChatAckMs"]),
            "commitUs": quantiles(trace["serverQuantiles"]["commitUs"]),
            "paired": paired,
            "covered": covered,
            "slow": slow,
            "slowCovered": slow_covered,
            "slowThresholdMs": number(observed["slowCommitThresholdMs"], 1),
            "slowWaitSamples": wait_totals,
            "waitPolls": wait_polls,
            "sampleIntervalMs": number(observed["sampleIntervalMs"], 1),
            "maxPollGapMs": number(observed["maxPollGapMs"]) if wait_polls >= 2 else None,
            "sampleErrors": count(observed["sampleErrors"]),
            "sampleLimitReached": poll_limit,
            "devices": devices,
            "longest": sorted(entries, key=lambda entry: entry["commitUs"], reverse=True),
            "diskDelta": disks(observed["workloadDiskDelta"], devices),
            "walDelta": None if wal is None else {key: number(wal[key]) for key in WAL_FIELDS},
            "postgresSettings": settings(observed["postgresSettings"]),
        }
    except (KeyError, TypeError, ValueError, OverflowError):
        raise Invalid("提交诊断格式或采样计数无效") from None
