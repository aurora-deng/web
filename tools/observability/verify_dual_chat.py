"""Verify two owned NEBULA instances, cross-process chat and optional telemetry failure drills."""

import argparse
from contextlib import closing, contextmanager
import json
import math
import os
from pathlib import Path
import re
import secrets
import shlex
import signal
import subprocess
import time
from urllib.parse import urlencode

from .isolated_dual import require_isolated_network
from .probe import probe_http, trace_request
from .scenarios import read_observer
from .verify_gateway import ROOT, browser_fixture, eventually, exchange, expect, port_ok, require
from repro_runtime import DATABASE, clean_environment, require_tool


class OwnedLab:
    def __init__(self, directory):
        self.directory = directory.resolve()
        self.network = require_isolated_network()
        self.session = json.loads((self.directory/"session.json").read_text(encoding="utf-8"))
        require(self.session.get("networkIsolation") == self.network, "Session belongs to another network namespace")
        self.nodes = self.session["instances"]
        require(set(self.nodes) == {"nebula-a", "nebula-b"} and self.session["migration"] >= 25,
                "Start a migrated isolated lab with --dual-node")
        require(len({node["port"] for node in self.nodes.values()}) == 2 and all(
            port_ok(node["port"]) for node in self.nodes.values()), "Unsafe lab instance ports")
        self.owner = int((self.directory/"lab.pid").read_text())
        self.require_owner()

    def require_owner(self):
        owner = Path(f"/proc/{self.owner}")
        require(os.readlink(owner/"ns/net") == self.network["labNamespace"],
                "Lab owner is in another network namespace")
        args = os.fsdecode((owner/"cmdline").read_bytes()).split("\0")
        require("tools.observability.lab" in args and "--dual-node" in args and "--work-dir" in args,
                "Isolated lab owner is not active")
        work_path = Path(args[args.index("--work-dir")+1])
        require((owner/"cwd"/work_path).resolve() == self.directory, "Session belongs to a different lab")

    def children(self):
        self.require_owner()
        return [int(pid) for pid in Path(f"/proc/{self.owner}/task/{self.owner}/children").read_text().split()]

    @contextmanager
    def child(self, pid, name):
        require(pid in self.children(), "Fault target must belong to this lab")
        descriptor = os.pidfd_open(pid)
        try:
            require(pid in self.children() and Path(f"/proc/{pid}/exe").resolve().name == name,
                    "Fault target identity changed")
            yield lambda event: signal.pidfd_send_signal(descriptor, event)
        finally:
            os.close(descriptor)

    def url(self, key):
        return f"http://127.0.0.1:{self.session['ports'][key]}"


def fixture_account(fixture, port, prefix):
    client = fixture.BrowserClient("127.0.0.1", port)
    username = prefix + secrets.token_hex(6)
    password = secrets.token_urlsafe(24)
    client.register_and_login(username, password)
    return client


def send(fixture, socket, reader, command, expected):
    start = time.monotonic_ns()
    socket.sendall(fixture.masked_frame(json.dumps(command).encode()))
    ack = fixture.wait_for_ws(reader, "chat.accepted", 50)
    require(ack.get("status") == expected and ack.get("replyTo") == command["id"]
            and re.fullmatch(r"[1-9][0-9]*", str(ack.get("id"))), "Unexpected persisted ACK")
    return ack, round((time.monotonic_ns()-start)/1e6, 3)


def peer_message(fixture, reader, key):
    for _ in range(50):
        row = fixture.wait_for_ws(reader, "chat.message", 50)
        if row.get("clientMessageId") == key:
            return row
    raise RuntimeError("The second process did not deliver this message")


def wait_notification(lab, status, predicate):
    return eventually(lambda: read_observer(lab.url("alert_receiver"), "/notifications")["notifications"],
        lambda rows: any(item["status"] == status and predicate(item) for row in rows for item in row["alerts"]),
        f"Did not receive {status} notification", timeout=100)


def notifications(lab):
    primary = lab.nodes["nebula-a"]

    def matches(item):
        return item["labels"].get("alertname") == "NebulaUnreachable" and item["labels"].get("instance") == "nebula-a"

    with lab.child(primary["pid"], "webserver") as send_signal:
        send_signal(signal.SIGSTOP)
        try:
            wait_notification(lab, "firing", matches)
            require(probe_http(lab.nodes["nebula-b"]["port"], "/health/ready")[0] == 200,
                    "Replica failed during primary pause")
            print("PRIMARY_UNREACHABLE_NOTIFIED", flush=True)
        finally:
            send_signal(signal.SIGCONT)
    observed = wait_notification(lab, "resolved", matches)
    print("PRIMARY_RECOVERY_NOTIFIED", flush=True)
    return [item for row in observed for item in row["alerts"] if matches(item)]


def trace_in(lab, client, instance):
    ports = lab.session["ports"]
    otlp = lab.url("collector" if "collector" in ports else "observer")
    trace, status = trace_request(client.port, otlp, client.cookie)
    require(status == 200, "Real conversation probe failed")

    def matches(data):
        spans = data["spans"]
        return (data["status"] == "complete" and any(row["name"] == "PostgreSQL command" for row in spans)
                and any(row["name"] == "GET /api/conversations" and
                        row["resourceAttributes"].get("service.instance.id") == instance for row in spans))

    detail = eventually(lambda: read_observer(lab.url("observer"), "/api/v1/traces/"+trace), matches,
                        "No complete instance-scoped trace", timeout=30)
    expected_source = "tempo" if "collector" in ports else "local"
    require(detail["source"] == expected_source, "Trace was not read from the expected backend")
    return {"id": trace, "backend": detail["source"], "spans": len(detail["spans"])}


def collector_outage(lab, client):
    require("collector" in lab.session["ports"] and lab.session["httpTraceEvery"] == 1,
            "Collector exercise requires the full profile and --http-trace-every 1")
    matching = [pid for pid in lab.children() if Path(f"/proc/{pid}/exe").resolve().name == "otelcol"]
    require(len(matching) == 1, "No unique owned Collector")
    token = (lab.directory/"scrape-token").read_text(encoding="utf-8").strip()

    def dropped():
        status, body = probe_http(client.port, "/metrics", {"Authorization": "Bearer "+token})
        require(status == 200, "Application metrics stopped responding")
        return next(int(row.split()[1]) for row in body.splitlines() if row.startswith(b"nebula_telemetry_dropped_total "))

    before = dropped()
    with lab.child(matching[0], "otelcol") as send_signal:
        send_signal(signal.SIGSTOP)
        try:
            for _ in range(18):
                expect(client, "GET", "/api/conversations")
            after = eventually(dropped, lambda count: count > before,
                               "Exporter did not account for drops during the outage", timeout=15)
        finally:
            send_signal(signal.SIGCONT)
    return {"successfulRequestsWhileStopped": 18, "applicationDrops": after-before,
            "recoveredTrace": trace_in(lab, client, "nebula-b")}


def replica_context(lab, primary, fixture):
    return fixture.BrowserClient("127.0.0.1", lab.nodes["nebula-b"]["port"], cookie=primary.cookie,
                                 csrf=primary.csrf, user_id=primary.user_id)


def gateway_membership(lab):
    status, text = probe_http(lab.session["ports"]["gateway_admin"], "/stats/prometheus")
    require(status == 200, "Gateway metrics are unavailable")
    match = re.search(rb'^envoy_cluster_membership_healthy\{[^}]*envoy_cluster_name="nebula_application"[^}]*\}\s+(\d+)(?:\.0+)?$',
                      text, re.MULTILINE)
    require(match is not None, "No healthy membership gauge for nebula_application")
    return int(match[1])


def gateway_route(lab, client):
    status, _, headers = exchange(client, "GET", "/api/conversations")
    trace = headers.get("x-nebula-trace-id", "")
    require(status == 200 and re.fullmatch(r"[0-9a-f]{32}", trace), "Missing sampled gateway request")

    def instances():
        data = read_observer(lab.url("observer"), "/api/v1/traces/" + trace)
        return {span["resourceAttributes"].get("service.instance.id") for span in data["spans"]
                if span["name"] == "GET /api/conversations"}

    used = eventually(instances, lambda rows: len(rows) == 1 and rows <= set(lab.nodes),
                      "Gateway request did not resolve to a lab instance")
    return used.pop()


def persisted_rows(lab, postgres_prefix, conversation, key):
    process = lab.nodes["nebula-b"]["pid"]
    require(process in lab.children(), "Database must belong to a live owned replica")
    env = Path(f"/proc/{process}/environ").read_bytes().split(b"\0")
    connection = next(row.split(b"=", 1)[1].decode() for row in env if row.startswith(b"WEB_PHASE11_DATABASE="))
    properties = dict(item.split("=", 1) for item in shlex.split(connection))
    require(re.fullmatch(r"/tmp/nebula-pg-[A-Za-z0-9_]+", properties.get("host", ""))
            and properties.get("dbname") == DATABASE and re.fullmatch(r"[a-z0-9-]+", key),
            "Database connection is outside the temporary lab")
    sql = ("SELECT count(*), COALESCE(min(id),0) FROM messages "
           f"WHERE conversation_id={int(conversation)} AND client_message_id='{key}'")
    psql = require_tool(postgres_prefix.resolve()/"bin/psql")
    return subprocess.check_output([psql, "-X", "-At", "-w", "-d", connection, "-c", sql],
                                   env=clean_environment(), text=True, timeout=5).strip().split("|")


def cross_instance(lab, args):
    fixture = browser_fixture(ROOT)
    author = fixture_account(fixture, lab.nodes["nebula-a"]["port"], "duala_")
    peer = fixture_account(fixture, lab.nodes["nebula-b"]["port"], "dualb_")
    invite = expect(author, "POST", "/api/friend-requests", {"receiverId": str(peer.user_id)}, expected=201)
    expect(peer, "POST", f"/api/friend-requests/{invite['id']}", {"accept": "true"})
    conversation = expect(author, "POST", "/api/conversations",
                          {"kind": "direct", "peerId": str(peer.user_id)}, expected=201)["id"]
    traces = {"nebula-a": trace_in(lab, author, "nebula-a"), "nebula-b": trace_in(lab, peer, "nebula-b")} if args.trace else None
    if traces is not None:
        print("DUAL_TRACES_VERIFIED", flush=True)
    ingress = None
    gateway_before = None
    if args.through_gateway:
        port = lab.session["ports"].get("gateway")
        require(port_ok(port) and port_ok(lab.session["ports"].get("gateway_admin")), "Start the dual lab with Envoy")
        require(lab.session["httpTraceEvery"] == 1, "Sample all isolated requests to identify their upstream")
        ingress = fixture.BrowserClient("127.0.0.1", port, cookie=author.cookie, csrf=author.csrf,
                                        user_id=author.user_id, origin=f"http://127.0.0.1:{port}")
        eventually(lambda: gateway_membership(lab), lambda count: count == 2, "Envoy did not find both upstreams healthy")
        gateway_before = set()
        for _ in range(12):
            gateway_before.add(gateway_route(lab, ingress))
            if gateway_before == set(lab.nodes):
                break
        require(gateway_before == set(lab.nodes), "Envoy did not send requests to both NEBULA instances")
        print("DUAL_GATEWAY_BALANCED", flush=True)
    interrupted_telemetry = collector_outage(lab, peer) if args.collector_outage else None
    if interrupted_telemetry is not None:
        print("COLLECTOR_RECOVERY_VERIFIED", flush=True)
    notices = notifications(lab) if args.notifications else None
    nonce = "dual-"+secrets.token_hex(8)
    command = {"v": 1, "type": "chat.send", "conversationId": str(conversation), "content": "isolated dual-node exercise"}
    samples = []
    crashed_at = None
    writer, writer_ws = author.open_websocket()
    listener, listener_ws = peer.open_websocket()
    with closing(writer), closing(listener):
        for index in range(1, args.messages+1):
            key = f"{nonce}-{index}"
            ack, latency = send(fixture, writer, writer_ws, {**command, "id": key}, "inserted")
            delivered = peer_message(fixture, listener_ws, key)
            require(str(delivered["id"]) == str(ack["id"]), "Peer received a different persisted message")
            samples.append(latency)
        final = {**command, "id": nonce+"-last"}
        ack, _ = send(fixture, writer, writer_ws, final, "inserted")
        if args.crash_primary:
            primary = lab.nodes["nebula-a"]
            require(lab.session.get("allowPrimaryCrash"), "Use --allow-primary-crash when starting the owned lab")
            with lab.child(primary["pid"], "webserver") as send_signal:
                crashed_at = time.monotonic()
                send_signal(signal.SIGKILL)
            eventually(lambda: probe_http(primary["port"], "/health/live")[0], lambda status: status == 0,
                       "Primary stopped responding unexpectedly slowly")
        delivered = peer_message(fixture, listener_ws, final["id"])
        require(str(delivered["id"]) == str(ack["id"]), "Lost live delivery at the failure boundary")
    failover = None
    recovered = replica_context(lab, author, fixture)
    if ingress is not None:
        eventually(lambda: gateway_membership(lab), lambda count: count == 1, "Envoy did not eject the failed upstream")
        observed_ms = round((time.monotonic() - crashed_at) * 1000, 3)
        routed = [gateway_route(lab, ingress) for _ in range(4)]
        require(routed == ["nebula-b"] * 4, "A new gateway request did not reach the healthy replica")
        failover = {"beforeInstances": sorted(gateway_before), "afterInstances": routed,
                    "healthyMembershipAfterCrash": 1, "observedEjectionMs": observed_ms}
        recovered = ingress
    history = expect(recovered, "GET", f"/api/conversations/{conversation}/messages?after=0&limit=100")
    require(sum(message.get("clientMessageId") == final["id"] for message in history) == 1,
            "Persisted message was not recovered exactly once")
    recovery_socket, recovery_reader = recovered.open_websocket()
    with closing(recovery_socket):
        duplicate, _ = send(fixture, recovery_socket, recovery_reader, final, "duplicate")
    require(str(duplicate["id"]) == str(ack["id"]), "Idempotency ID changed across instances")
    rows, stored_id = persisted_rows(lab, args.postgres_prefix, conversation, final["id"])
    require(rows == "1" and stored_id == str(ack["id"]), "Message duplication in PostgreSQL")
    latency = sorted(samples)
    return {"liveMessages": len(samples)+1, "ackSamples": len(samples), "ackMilliseconds": samples,
            "ackP95NearestRankMs": latency[math.ceil(len(latency)*.95)-1], "primaryKilled": args.crash_primary,
            "recoveredRows": int(rows), "recoveredMessageId": stored_id,
            "traces": traces, "collectorOutage": interrupted_telemetry, "notifications": notices,
            "gatewayFailover": failover}


def check_metrics(lab):
    if "prometheus" not in lab.session["ports"]:
        return None
    observer = lab.url("observer")
    scopes = eventually(lambda: read_observer(observer, "/api/v1/scopes")["items"],
        lambda rows: {item["instance"] for item in rows} == set(lab.nodes), "Both instance labels did not appear")
    # Target discovery can advertise labels before the first business metrics scrape.
    def replica_objectives():
        end = int(time.time())
        return read_observer(observer, "/api/v1/service-levels?"+urlencode({
            "from": end-300, "to": end, "instance": "nebula-b"}))

    data = eventually(replica_objectives,
        lambda row: row.get("instance") == "nebula-b" and len(row.get("objectives", [])) == 3,
        "Prometheus did not expose the replica service objectives after target discovery")
    return {"scopes": scopes, "objectives": [row["id"] for row in data["objectives"]]}


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--run-dir", required=True, type=Path)
    cli.add_argument("--postgres-prefix", required=True, type=Path)
    cli.add_argument("--output", required=True, type=Path)
    cli.add_argument("--messages", type=int, default=24)
    cli.add_argument("--trace", action="store_true")
    cli.add_argument("--notifications", action="store_true")
    cli.add_argument("--collector-outage", action="store_true")
    cli.add_argument("--crash-primary", action="store_true")
    cli.add_argument("--through-gateway", action="store_true", help="Verify owned Envoy balancing and post-crash replay")
    args = cli.parse_args()
    require(1 <= args.messages <= 80, "Use a bounded 1–80 message fixture")
    require(not args.through_gateway or args.crash_primary, "Gateway failover requires --crash-primary")
    require(not (args.crash_primary or args.notifications or args.collector_outage) or hasattr(os, "pidfd_open"),
            "Linux pidfd is required for owned fault exercises")
    require(not args.output.exists(), "Choose a fresh report path")
    lab = OwnedLab(args.run_dir)
    metrics = check_metrics(lab)
    if metrics is not None:
        print("DUAL_METRICS_VERIFIED", flush=True)
    result = {"metrics": metrics, **cross_instance(lab, args), "passed": True}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as report:
        json.dump(result, report, indent=2)
    print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
