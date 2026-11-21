"""Read models over persisted telemetry. The scheduler remains the source of truth."""
import collections

RESOURCE_KEYS = ("service.name", "service.namespace", "service.instance.id", "deployment.environment.name", "host.name",
                 "k8s.cluster.name", "k8s.namespace.name", "k8s.node.name", "k8s.pod.name")


def trace_view(raw):
    trace = raw.get("trace", raw)
    spans = {}
    for group in trace.get("resourceSpans", trace.get("batches", [])):
        attrs = {a["key"]: next(iter(a.get("value", {}).values()), None)
                 for a in group.get("resource", {}).get("attributes", [])}
        for scope in group.get("scopeSpans", group.get("instrumentationLibrarySpans", [])):
            for span in scope.get("spans", []):
                ident = span["spanId"]
                tags = {a["key"]: next(iter(a.get("value", {}).values()), None) for a in span.get("attributes", [])}
                retained = ("http.request.method","http.response.status_code","rpc.system","db.system","error.type")
                spans[ident] = {"id": ident, "parent": span.get("parentSpanId"),
                                "name": span["name"], "service": attrs.get("service.name", ""),
                                "start": span["startTimeUnixNano"], "end": span["endTimeUnixNano"],
                                "links": [link["spanId"] for link in span.get("links", [])],
                                "place": " / ".join(str(attrs[key]) for key in
                                    ("deployment.environment.name","service.instance.id","k8s.cluster.name","k8s.namespace.name","k8s.node.name","host.name","k8s.pod.name")
                                    if attrs.get(key)),
                                "attributes": {key: tags[key] for key in retained if key in tags},
                                "resourceAttributes": {key: attrs[key] for key in RESOURCE_KEYS if key in attrs},
                                "cpuNs": tags.get("nebula.thread_cpu_ns"),
                                "kind": span.get("kind"),
                                "status": span.get("status", {}).get("code", 0)}
    return sorted(spans.values(), key=lambda s: (int(s["start"]), s["id"]))


def is_partial(raw, spans):
    if not spans:
        return True
    status = str(raw.get("status","")).lower()
    ids = {s["id"] for s in spans}
    return (status == "2" or status.endswith("partial") or
            any(s["parent"] and s["parent"] not in ids for s in spans))


def device_summary(items):
    chains = {}
    for item in items:
        f = item["fields"]
        if "commandId" not in f:
            continue
        name = (item["source"], f["deviceId"], f["sessionId"], f["commandId"])
        row = chains.setdefault(name, {"id": f["commandId"], "device": f["deviceId"],
                                       "session": f["sessionId"], "states": []})
        row["states"].append(f["state"])
        if f["state"] == "ack" and "durationMs" in f:
            row["ackMs"] = f["durationMs"]
    return list(chains.values())


def agent_summary(items):
    latest, attempts, tokens = {}, set(), collections.Counter()
    missing = 0
    for event in items:
        f = event["fields"]
        if event["eventType"] == "agent.skill" and f["state"] in {"success", "failed", "timeout", "invalid", "cancelled"}:
            key = (event["source"], f["agentRunId"], f["callId"])
            attempt = (*key, f["attempt"])
            if attempt in attempts:
                continue
            attempts.add(attempt)
            if key not in latest or latest[key]["attempt"] < f["attempt"]:
                latest[key] = f
        elif event["eventType"] != "agent.plan":
            continue
        if event["eventType"] == "agent.skill" and not any(k in f for k in ("inputTokens", "outputTokens")):
            # The tool may not be a model. Only explicit model/plan usage participates.
            missing += 2
            continue
        for direction in ("inputTokens", "outputTokens"):
            if direction in f:
                tokens[direction] += f[direction]
            else:
                missing += 1
    skills = {}
    for value in latest.values():
        row = skills.setdefault(value["skill"], {"finished": 0, "succeeded": 0})
        row["finished"] += 1
        row["succeeded"] += int(value["state"] == "success")
    return {"tokens": dict(tokens), "unknownTokenFields": missing, "skills": skills,
            "attempts": len(attempts), "cost": None}


def agent_cost(summary, pricing):
    # One explicitly configured development rate. Mixed models need per-model usage first.
    if pricing and not summary["unknownTokenFields"] and summary["tokens"]:
        value = sum(summary["tokens"].get(k, 0) * pricing[k] for k in ("inputTokens","outputTokens"))
        summary["cost"] = {"currency": pricing["currency"], "estimate": round(value / 1_000_000, 8)}
    return summary


def compare(a, b):
    shared = ("configSha256", "workloadSha256", "fixtureSha256", "environmentSha256")
    mismatch = [k for k in shared if not a["identity"].get(k) or a["identity"][k] != b["identity"].get(k)]
    for r in (a, b):
        if (not (r["identity"].get("commit") or r["identity"].get("sourceFingerprint"))
                or not r["identity"].get("binarySha256")):
            mismatch.append("buildIdentity")
        if (r["result"].get("ackSamples", 0) < 1000 or r["result"].get("errors") != 0
                or r["result"].get("ackP99Ms", 0) <= 0):
            mismatch.append("samplesOrErrors")
    if a["scenario"] != b["scenario"] or a["result"].get("workers") != b["result"].get("workers"):
        mismatch.append("workload")
    if mismatch:
        return {"status": "incomparable", "reasons": sorted(set(mismatch))}
    delta = (b["result"]["ackP99Ms"] / a["result"]["ackP99Ms"] - 1) * 100
    return {"status": "comparable", "ackP99DeltaPercent": round(delta, 3)}
