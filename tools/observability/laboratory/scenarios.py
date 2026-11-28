"""Reproducible producers that send real OTLP and Observer events."""
import argparse
import collections
import json
from pathlib import Path
import secrets
import time
from urllib.parse import urlencode
import urllib.request

from tools.observability.observer.storage.store import encode


def post(base, path, body, token=None):
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    request = urllib.request.Request(base + path, encode(body).encode(), headers)
    with urllib.request.urlopen(request, timeout=4) as reply:
        return json.load(reply)


def read_observer(base,path):
    with urllib.request.urlopen(base+path,timeout=4) as reply:
        return json.load(reply)


def span(trace, name, start, end, parent=None, links=(), failed=False, kind=1, span_id=None):
    data = {"traceId": trace, "spanId": span_id or secrets.token_hex(8), "name": name, "kind": kind,
            "startTimeUnixNano": str(start), "endTimeUnixNano": str(end), "status": {"code": 2 if failed else 1}}
    if parent:
        data["parentSpanId"] = parent
    if links:
        data["links"] = [{"traceId": trace, "spanId": x} for x in links]
    return data


def export(otlp, service, spans):
    payload = {"resourceSpans": [{"resource": {"attributes": [
        {"key": "service.name", "value": {"stringValue": service}}]},
        "scopeSpans": [{"scope": {"name": "nebula.lab"}, "spans": spans}]}]}
    result = post(otlp, "/v1/traces", payload)
    if result.get("partialSuccess", {}).get("rejectedSpans", 0):
        raise RuntimeError("OTLP receiver rejected spans")


class Scenario:
    def __init__(self, prefix):
        self.trace = secrets.token_hex(16)
        self.ident = prefix + "-" + secrets.token_hex(5)
        self.start = time.time_ns() - 2 * 10**9
        self.events = []
        self.spans = []

    def event(self, kind, fields, ms=0, span_id=None):
        body = {"schemaVersion": 1, "eventId": self.ident + "-" + str(len(self.events) + 1),
                "eventType": kind, "occurredAt": str(self.start + int(ms * 1e6)), "fields": fields}
        if not kind.startswith("device."):
            body["traceId"] = self.trace
        if span_id:
            body["spanId"] = span_id
        self.events.append(body)

    def span(self, name, start, end, parent=None, links=(), failed=False):
        data = span(self.trace, name, self.start + start * 10**6, self.start + end * 10**6,
                    parent, links, failed)
        self.spans.append(data)
        return data["spanId"]


def dag(skip=False):
    lab = Scenario("dag")
    root = lab.span("workflow.example", 0, 980, failed=skip)
    lab.event("dag.run", {"runId": lab.ident, "state": "started", "workflow": "join-example"})
    for before, after in (("A", "B"), ("A", "C"), ("B", "D"), ("C", "D")):
        lab.event("dag.edge", {"runId": lab.ident, "predecessor": before, "successor": after})

    def node(name, attempt, start, end, state, parents=()):
        sid = lab.span(name, start, end, root, parents, state in ("failed", "timeout"))
        attrs = {"runId": lab.ident, "nodeId": name, "attempt": attempt}
        lab.event("dag.node", {**attrs, "state": "queued"}, max(0, start - 15), sid)
        lab.event("dag.node", {**attrs, "state": "started", "inputCount": len(parents)}, start, sid)
        lab.event("dag.node", {**attrs, "state": state, "durationMs": end - start,
                               "outputCount": int(state == "success")}, end, sid)
        return sid

    a = node("A", 1, 20, 150, "success")
    node("B", 1, 175, 330, "failed", (a,))
    lab.event("dag.node", {"runId": lab.ident, "nodeId": "B", "attempt": 1, "state": "retry"}, 360,
              lab.spans[-1]["spanId"])
    b = node("B", 2, 370, 620, "timeout" if skip else "success", (a,))
    c = node("C", 1, 180, 440, "success", (a,))
    if skip:
        sid = lab.span("D", 635, 635, root, (b, c))
        lab.event("dag.node", {"runId": lab.ident, "nodeId": "D", "attempt": 1, "state": "skipped"}, 635, sid)
    else:
        node("D", 1, 635, 900, "success", (b, c))
    lab.event("dag.run", {"runId": lab.ident, "state": "failed" if skip else "success"}, 980)
    return lab


def agent():
    lab = Scenario("agent")
    root = lab.span("agent.plan-and-execute", 0, 720)
    plan = lab.span("agent.plan", 5, 135, root)
    lab.event("agent.plan", {"agentRunId": lab.ident, "state": "success", "durationMs": 130,
                            "stepCount": 3, "inputTokens": 120, "outputTokens": 43}, 135, plan)
    for attempt, start, end, state in ((1, 140, 230, "invalid"), (2, 360, 570, "success")):
        sid = lab.span("skill.search", start, end, root, failed=state != "success")
        fields = {"agentRunId": lab.ident, "callId": "search-1", "skill": "search", "attempt": attempt}
        lab.event("agent.skill", {**fields, "state": "started", "inputCount": 1}, start, sid)
        lab.event("agent.skill", {**fields, "state": state, "durationMs": end-start, "inputCount": 1,
                                 "outputCount": int(state == "success")}, end, sid)
        if state == "invalid":
            lab.event("agent.skill", {**fields, "state": "retry"}, end + 1, sid)
    for i, (kind, fields) in enumerate((
        ("agent.memory", {"operation": "read", "state": "success", "itemCount": 2}),
        ("agent.memory", {"operation": "write", "state": "blocked"}),
        ("agent.decision", {"decision": "permission", "state": "blocked", "reasonCode": "no_scope"}),
        ("agent.decision", {"decision": "refusal", "state": "blocked", "reasonCode": "policy"}),
        ("agent.evaluation", {"evaluator": "fixture", "state": "flagged", "reasonCode": "unsupported_claim"}),
    )):
        lab.event(kind, {"agentRunId": lab.ident, **fields}, 600 + i * 20, root)
    return lab


class Frames:
    """Hold at most the next N sequence positions. A skipped frame remains a gap."""
    def __init__(self, first, lag=3):
        self.first = self.next = first
        self.high = first - 1
        self.lag = lag
        self.pending = set()
        self.seen = collections.deque(maxlen=lag * 2 + 1)
        self.samples = self.gaps = self.duplicates = self.late = 0
        self.minimum = self.maximum = None

    def add(self, seq, value):
        if seq in self.pending or seq in self.seen:
            self.duplicates += 1
            return
        if seq < self.next:
            self.late += 1
            return
        self.pending.add(seq)
        self.high = max(self.high, seq)
        self.samples += 1
        self.minimum = value if self.minimum is None else min(self.minimum, value)
        self.maximum = value if self.maximum is None else max(self.maximum, value)
        self.advance(self.high - self.lag)

    def advance(self, limit):
        for seq in sorted(s for s in self.pending if s <= limit):
            self.gaps += seq - self.next
            self.next = seq + 1
            self.pending.remove(seq)
            self.seen.append(seq)
        if self.next <= limit:
            self.gaps += limit - self.next + 1
            self.next = limit + 1


def device():
    lab = Scenario("device")
    for count, (stream, values) in enumerate((
        ("camera", [0, 2, 1, 1, 4, 7, 3]),
        ("imu", [0, 1, 3, 2, 4, 8, 1]),
        ("imu", [0, 1, 3, 2]),
    )):
        window = Frames(0)
        for seq in values:
            window.add(seq, seq / 10)
        window.advance(window.high)
        lab.event("device.window", {"deviceId": "robot-01", "sessionId": "boot-02" if count == 2 else "boot-01",
                                    "stream": stream, "firstSeq": str(window.first), "lastSeq": str(window.high),
                                    "samples": window.samples, "gaps": window.gaps, "duplicates": window.duplicates,
                                    "late": window.late, "clockQuality": "unsynchronized" if count == 2 else "synchronized",
                                    "capturedAt": str(lab.start + (350_000_000 if count == 2 else 1_200_000)),
                                    "offsetMs": 350 if count == 2 else 1.2, "minValue": window.minimum, "maxValue": window.maximum}, count+1)
    common = {"deviceId": "robot-01", "sessionId": "boot-02", "commandId": lab.ident}
    lab.event("device.command", {**common, "state": "sent", "action": "move"}, 90)
    lab.event("device.command", {**common, "state": "ack", "durationMs": 30}, 120)
    lab.event("device.command", {**common, "state": "error", "errorCode": "obstacle"}, 140)
    lab.event("device.safety", {**common, "state": "estop", "reasonCode": "obstacle"}, 150)
    return lab


def main():
    cli = argparse.ArgumentParser()
    cli.add_argument("scenario", choices=("dag", "dag-failed", "agent", "device"))
    cli.add_argument("--credentials", type=Path, required=True)
    cli.add_argument("--observer", default="http://127.0.0.1:18785")
    cli.add_argument("--otlp", default="http://127.0.0.1:14318")
    cli.add_argument("--events-only", action="store_true", help="Exercises the event path without Tempo")
    cli.add_argument("--verify", action="store_true", help="Read the uploaded event IDs back from Observer")
    args = cli.parse_args()
    builders = {"dag": dag, "dag-failed": lambda: dag(skip=True), "agent": agent, "device": device}
    scenario = builders[args.scenario]()
    domain = args.scenario.split("-")[0]
    token = json.loads(args.credentials.read_text(encoding="utf-8"))[domain + "-sim"]["token"]
    if scenario.spans and not args.events_only:
        export(args.otlp, domain + "-sim", scenario.spans)
    outcome = post(args.observer, "/ingest/v1/events", scenario.events, token)
    repeated = post(args.observer, "/ingest/v1/events", scenario.events, token)
    if repeated != {"inserted": 0, "duplicates": len(scenario.events)}:
        raise RuntimeError("Duplicate batch was not idempotent")
    query = urlencode({"domain":domain,"source":domain+"-sim","from":scenario.start//10**9-1,"to":int(time.time())+1,"limit":200})
    result_path = "/?"+urlencode({"view":"extensions","domain":domain,"source":domain+"-sim"})
    if domain != "device":
        result_path = "/?view=traces&trace="+scenario.trace
    verified = None
    if args.verify:
        received = read_observer(args.observer,"/api/v1/events?"+query)["items"]
        expected = {item["eventId"] for item in scenario.events}
        verified = len(expected & {item["eventId"] for item in received
                       if item["source"]==domain+"-sim" and item["simulated"]})
        if verified != len(expected) or outcome["inserted"] != len(expected):
            raise RuntimeError("Observer read-back did not match the uploaded events")
    print(encode({"scenario": args.scenario, "traceId": scenario.trace if domain != "device" else None,
                  "first": outcome, "repeat": repeated, "readBack":verified,"path":result_path,
                  "simulated": True, "otlp": bool(scenario.spans and not args.events_only)}))


if __name__ == "__main__":
    main()
