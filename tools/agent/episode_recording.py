"""Checksummed synthetic state/range recordings for fleet replay and training."""

from dataclasses import dataclass
from hashlib import sha256
import json
from pathlib import Path
from random import Random
import re


FORMAT = "nebula.synthetic-episode.v1"
TICK_NS = 100_000_000
MAX_SKEW_NS = 40_000_000
MAX_BYTES = 2 * 1024 * 1024
SESSION_ID = re.compile(r"[a-z0-9-]{1,40}")
STATIONARY = 0
STEP = 1


@dataclass(frozen=True)
class Observation:
    session: str
    seq: int
    remaining: int
    clearance: int

    def expert(self):
        return STEP if self.remaining and self.clearance >= 60 else STATIONARY


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("ascii")


def _read(path, limit):
    with path.open("rb") as source:
        data = source.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f"{path.name} is over budget")
    return data


def fixture(root, seed=20261123):
    """Create a 90-second diagnostics stream plus disjoint train/eval sessions."""
    root = Path(root)
    root.mkdir(parents=True)
    rng = Random(seed)
    frames, sessions = [], []
    groups = [(f"train-{i}", "train", 32) for i in range(24)]
    groups += [(f"eval-{i}", "eval", 32) for i in range(12)]
    groups += [("diagnostics", "diagnostic", 900), ("clock-mismatch", "quarantine", 8)]
    for name, group, count in groups:
        trace = sha256(f"{seed}:{name}".encode()).hexdigest()[:32]
        sessions.append({"id": name, "split": group, "count": count, "taskId": f"task-{name}",
                         "deviceId": "sim-alpha", "traceId": trace, "software": "grid.v1",
                         "clockQuality": "degraded" if group == "quarantine" else "synchronized"})
        remaining = 22
        rows = []
        for seq in range(count):
            clearance = rng.randrange(100)
            if group == "diagnostic" and not remaining:
                remaining = 22
            state = {"session": name, "stream": "state", "seq": seq, "ns": seq * TICK_NS, "value": remaining}
            sensor = {**state, "stream": "range", "value": clearance,
                      "ns": state["ns"] + 5_000_000 + rng.randrange(10_000_000)}
            if group == "diagnostic" and seq == 35:
                sensor["ns"] += 70_000_000
            rows.append(state)
            if group != "diagnostic" or seq not in (17, 42):
                rows.append(sensor)
            remaining -= Observation(name, seq, remaining, clearance).expert()
        if group == "diagnostic":
            late = rows.pop(13)
            rows.insert(40, late)
            rows.insert(24, dict(rows[3]))
        frames.extend(rows)
    data = b"\n".join(encoded(row) for row in frames) + b"\n"
    digest = sha256(data).hexdigest()
    manifest = {"format": FORMAT, "simulated": True, "permittedUse": "local-simulation-and-learning", "seed": seed,
                "recordingId": f"sim-{digest[:20]}", "sha256": digest, "bytes": len(data), "sessions": sessions}
    (root / "frames.jsonl").write_bytes(data)
    (root / "manifest.json").write_bytes(encoded(manifest) + b"\n")
    return manifest


def replay(root):
    root = Path(root)
    manifest = json.loads(_read(root / "manifest.json", 65536))
    data = _read(root / "frames.jsonl", MAX_BYTES)
    digest = sha256(data).hexdigest()
    if (manifest["format"] != FORMAT or manifest["simulated"] is not True or manifest["bytes"] != len(data) or
            manifest["sha256"] != digest or manifest["recordingId"] != f"sim-{digest[:20]}"):
        raise ValueError("recording checksum or format mismatch")
    sessions = {}
    for item in manifest["sessions"]:
        name = item["id"]
        if (not SESSION_ID.fullmatch(name) or name in sessions or item["split"] not in
                {"train", "eval", "diagnostic", "quarantine"} or not 1 <= item["count"] <= 900 or
                item["clockQuality"] not in {"degraded", "synchronized"}):
            raise ValueError("invalid session metadata")
        sessions[name] = item
    if not 1 <= len(sessions) <= 64:
        raise ValueError("recording exceeds session budget")

    unique = {}
    highest = {}
    sequence = {name: {stream: {"received": 0, "unique": 0, "duplicate": 0, "late": 0, "gaps": 0}
                       for stream in ("state", "range")} for name in sessions}
    for line in data.splitlines():
        if len(line) > 1024:
            raise ValueError("oversized recording frame")
        row = json.loads(line)
        session, stream, seq, timestamp, value = (row[k] for k in ("session", "stream", "seq", "ns", "value"))
        if (session not in sessions or stream not in ("state", "range") or type(seq) is not int or
                not 0 <= seq < sessions[session]["count"] or type(timestamp) is not int or
                not seq * TICK_NS <= timestamp < (seq + 1) * TICK_NS or type(value) is not int or
                not 0 <= value <= (100 if stream == "range" else 22)):
            raise ValueError("invalid synthetic frame")
        stream_id = session, stream
        key = session, stream, seq
        counts = sequence[session][stream]
        counts["received"] += 1
        if key in unique:
            if unique[key] != row:
                raise ValueError("conflicting duplicate frame")
            counts["duplicate"] += 1
            continue
        if seq < highest.get(stream_id, -1):
            counts["late"] += 1
        highest[stream_id] = max(seq, highest.get(stream_id, -1))
        counts["unique"] += 1
        unique[key] = row

    aligned = {name: [] for name in sessions}
    rejected = {}
    for name, session in sessions.items():
        for counts in sequence[name].values():
            counts["gaps"] = session["count"] - counts["unique"]
        for seq in range(session["count"]):
            state = unique.get((name, "state", seq))
            sensor = unique.get((name, "range", seq))
            if state is None or sensor is None:
                reason = "missing_frame"
            elif session["clockQuality"] != "synchronized" or abs(state["ns"] - sensor["ns"]) > MAX_SKEW_NS:
                reason = "low_clock_quality"
            else:
                aligned[name].append(Observation(name, seq, state["value"], sensor["value"]))
                continue
            rejected[reason] = rejected.get(reason, 0) + 1
    action_rows = [[row.session, row.seq, row.expert()] for rows in aligned.values() for row in rows]
    return manifest, aligned, {"sequence": sequence, "rejected": rejected,
                               "actionSha256": sha256(encoded(action_rows)).hexdigest()}


def recording_events(manifest, quality, occurred_ns):
    session = next(item for item in manifest["sessions"] if item["id"] == "diagnostics")
    base = {"schemaVersion": 1, "traceId": session["traceId"], "occurredAt": str(occurred_ns)}
    attrs = {"deviceId": session["deviceId"], "sessionId": session["id"]}
    prefix = manifest["recordingId"]
    events = [{**base, "eventId": prefix + "-recording", "eventType": "device.recording",
               "fields": {**attrs, "recordingId": prefix, "stream": "state-range"}}]
    for stream, counts in quality["sequence"][session["id"]].items():
        events.append({**base, "eventId": prefix + "-" + stream, "eventType": "device.window",
                       "fields": {**attrs, "stream": stream, "samples": counts["unique"], "gaps": counts["gaps"],
                                  "duplicates": counts["duplicate"], "late": counts["late"],
                                  "clockQuality": session["clockQuality"]}})
    return events
