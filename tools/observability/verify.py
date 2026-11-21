"""Exercise the real business service and optional full stack from synthetic accounts."""
import argparse
from contextlib import contextmanager
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import secrets
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts/project"))
from repro_runtime import clean_environment, disposable_database, owned_server, require
from .observer.store import encode
from .observer.sources import Sources
from .observer.server import ObserverServer
from .observer.store import Store
from .probe import probe_http
from .scenarios import agent, dag, device, post


@contextmanager
def otlp_capture():
    # A wire-format receiver used to verify the C++ exporter when third-party
    # binaries are not provisioned. Data remains local and synthetic.
    messages = []
    ready = threading.Event()
    ready.set()

    class Receiver(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            if self.path != "/v1/traces" or self.headers.get_content_type() != "application/json":
                self.send_error(400)
                return
            if not 0 < int(self.headers.get("Content-Length", "0")) < 256 * 1024:
                self.send_error(413)
                return
            payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if not ready.is_set():
                time.sleep(.08)
                self.send_error(503)
                return
            messages.append(payload)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b"{}")

    with HTTPServer(("127.0.0.1", 0), Receiver) as service:
        thread = threading.Thread(target=service.serve_forever)
        thread.start()
        try:
            yield service.server_port, messages, ready
        finally:
            service.shutdown()
            thread.join()


def token(secret, tenant):
    unsigned = f"v1.9000.{int(time.time()) + 60}.{tenant}"
    return unsigned + "." + hmac.new(secret.encode(), unsigned.encode(), hashlib.sha256).hexdigest()


def captured_spans(batches):
    return [span for batch in tuple(batches) for resource in batch["resourceSpans"]
            for scope in resource["scopeSpans"] for span in scope["spans"]]


def await_trace(batches, trace_id, seconds=12):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        spans = [span for span in captured_spans(batches) if span["traceId"] == trace_id]
        if len(spans) >= 3:
            parent = next((sp for sp in spans if sp["name"] == "probe GET /api/conversations"), {})
            server = next((sp for sp in spans if sp["name"] == "GET /api/conversations"), {})
            db = [sp for sp in spans if sp["name"] == "PostgreSQL command" and sp.get("parentSpanId") == server.get("spanId")]
            if server.get("spanId") and parent.get("spanId") and server.get("parentSpanId") == parent["spanId"] and db:
                return len(spans), len(db)
        time.sleep(.15)
    return 0, 0


def verify_http_sampling(prefix, connection, port, cookie, batches, metric_headers):
    from .alert_lab import sql, wait_for

    url = "/api/conversations"
    unparented = {"Cookie": cookie}
    upstream_unsampled = {**unparented, "traceparent": "00-" + secrets.token_hex(16) + "-" + secrets.token_hex(8) + "-00"}
    known_ids = {span["traceId"] for span in captured_spans(batches)}
    for headers in (unparented, upstream_unsampled):
        require(probe_http(port, url, headers)[0] == 200, "Sampling changed a successful HTTP response")

    # This table belongs to the disposable database. A missing relation gives a
    # deterministic 500 without waiting for lock or connection pool timeouts.
    sql(prefix, connection, "ALTER TABLE conversations RENAME TO observer_wire_conversations")
    try:
        for headers in (unparented, upstream_unsampled):
            require(probe_http(port, url, headers)[0] == 500, "Expected the isolated PostgreSQL failure to produce a 500")
    finally:
        sql(prefix, connection, "ALTER TABLE observer_wire_conversations RENAME TO conversations")

    def new_spans():
        return [span for span in captured_spans(batches) if span["traceId"] not in known_ids]

    wait_for(new_spans)
    require(probe_http(port, url, unparented)[0] == 200, "Sampling test did not recover after schema restoration")
    code, sample = probe_http(port, "/metrics", metric_headers)
    require(code == 200 and b"nebula_http_trace_sampling_skipped_total 2\n" in sample and
            b"nebula_http_trace_failure_roots_total 1\n" in sample, "HTTP sampling and failure counters disagree")
    roots = new_spans()
    expected = {"key": "http.response.status_code", "value": {"intValue": "500"}}
    require(len(roots) == 1 and roots[0]["name"] == "GET /api/conversations" and
            roots[0]["status"]["code"] == 2 and "parentSpanId" not in roots[0] and
            expected in roots[0]["attributes"], "Expected one failure root and no children or upstream-rejected spans")
    return {"locallySkipped": 2, "failureRoots": 1, "upstreamUnsampledHonored": True}


def verify_wire(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    with disposable_database(args.postgres_prefix.resolve(), ROOT, out) as (connection, migration):
        key = out / "token.key"
        key.touch(mode=0o600, exist_ok=False)
        key.write_text(secrets.token_hex(32), encoding="ascii")
        secret = secrets.token_hex(32)
        env = {**clean_environment(), "WEB_PHASE11_REQUIRED": "1", "WEB_FUNCTION3_ENABLE": "1",
               "WEB_PHASE11_DATABASE": connection, "WEB_PHASE11_TOKEN_KEY_FILE": str(key),
               "WEB_PHASE11_FILE_ROOT": str(out / "files"), "WEB_FUNCTION3_MEDIA_ROOT": str(out / "media"),
               "WEB_AUTH_SECRET": secret, "WEB_OBSERVABILITY": "1", "WEB_OBSERVABILITY_QUEUE_CAPACITY":"8"}
        env.update(WEB_OBSERVABILITY_HTTP_TRACE_EVERY="0", WEB_OBSERVABILITY_LOG="error")
        with otlp_capture() as (receiver_port, data, receiver_ready):
            address = "http://127.0.0.1:" + str(receiver_port)
            with owned_server(args.binary.resolve(), ROOT, {**env, "WEB_OBSERVABILITY_OTLP":address}, out / "nebula.log") as port:
                plain = probe_http(port, "/metrics")[0]
                member = probe_http(port, "/metrics", {"Authorization":"Bearer " + token(secret, "learning")})[0]
                headers = {"Authorization":"Bearer " + token(secret, "ops")}
                authorized, _ = probe_http(port, "/metrics", headers)
                require((plain, member, authorized) == (401, 403, 200), f"metrics authorization: {(plain,member,authorized)}")
                command = [sys.executable, "-m", "tools.observability.probe", "trace",
                           "--target-port", str(port), "--otlp", address]
                result = subprocess.check_output(command, cwd=ROOT, env=clean_environment(), text=True, timeout=15)
                trace_id = json.loads(result)["traceId"]
                total, db = await_trace(data, trace_id)
                require(total > 0, "No linked probe, actual server and PostgreSQL spans reached receiver")
                code, metrics = probe_http(port, "/metrics", headers)
                require(code == 200 and b'nebula_http_handler_seconds_count{route="conversations"} 1' in metrics,
                        "No route histogram sample after real request")
                from repro_runtime import browser_fixture
                user = browser_fixture(ROOT).BrowserClient("127.0.0.1", port)
                user.register_and_login("obs_fault_" + secrets.token_hex(4), secrets.token_urlsafe(16))
                sampling = verify_http_sampling(args.postgres_prefix.resolve(), connection, port, user.cookie, data, headers)
                receiver_ready.clear()
                request_headers = {"Cookie": user.cookie, "traceparent": "00-" + secrets.token_hex(16) + "-" + secrets.token_hex(8) + "-01"}
                for _ in range(8):
                    require(probe_http(port, "/api/conversations", request_headers)[0] == 200,
                            "Business request failed while telemetry receiver rejected uploads")
                until = time.monotonic() + 5
                while time.monotonic() < until:
                    code, sample = probe_http(port, "/metrics", headers)
                    count = next((int(x.split()[-1]) for x in sample.splitlines()
                                  if x.startswith(b"nebula_telemetry_dropped_total ")), 0)
                    full = next((int(x.split()[-1]) for x in sample.splitlines()
                                 if x.startswith(b"nebula_telemetry_queue_full_total ")), 0)
                    if code == 200 and full and count > full:
                        break
                    time.sleep(.1)
                else:
                    raise RuntimeError("No failed export was counted")
                receiver_ready.set()
                time.sleep(.3)  # Wait for the bounded failed batch to leave the sender.
                recovered = json.loads(subprocess.check_output(command, cwd=ROOT, env=clean_environment(),
                                                                text=True, timeout=15))["traceId"]
                require(await_trace(data, recovered)[0] > 0, "Exporter did not resume after receiver recovered")
                report = {"mode":"wire-receiver", "schema":migration, "unauthenticated":plain,
                          "nonOps":member, "ops":authorized, "probeStatus":200,
                          "matchedSpans":total, "postgreSqlSpans":db, "routeHistogram":True,
                          "receiverFailureBusinessStatus":200, "failedExportsCounted":count,
                          "queueFullDrops":full,"exportResumed":True,"httpSampling":sampling}
    (out / "report.json").write_text(encode(report) + "\n", encoding="utf-8")
    print(encode(report))


def verify_stack(args):
    session = json.loads((args.run_dir / "session.json").read_text())
    ports = session["ports"]
    otlp = f"http://127.0.0.1:{ports['collector']}"
    observer = f"http://127.0.0.1:{ports['observer']}"
    for command in ([sys.executable, "-m", "tools.observability.probe", "trace", "--target-port", str(session["nebulaPort"]), "--otlp", otlp],
                    *([sys.executable, "-m", "tools.observability.scenarios", example,
                       "--credentials", str(args.run_dir / "producers.json"), "--observer", observer, "--otlp", otlp]
                      for example in ("dag", "dag-failed", "agent", "device"))):
        raw = subprocess.check_output(command, cwd=ROOT, text=True, timeout=20)
        row = json.loads(raw)
        trace = row.get("traceId")
        if trace:
            until = time.monotonic()+55
            while time.monotonic()<until:
                code, body = probe_http(ports["observer"], "/api/v1/traces/"+trace)
                if code==200 and json.loads(body)["status"]=="complete":
                    result = json.loads(body)
                    names = {s["name"] for s in result["spans"]}
                    if "probeStatus" in row or row.get("status") == 200:
                        require({"GET /api/conversations","PostgreSQL command"} <= names,"Actual spans missing")
                    if row.get("scenario") == "dag":
                        require(len(next(s for s in result["spans"] if s["name"]=="D")["links"])==2,"DAG join links missing")
                    break
                time.sleep(.7)
            else:
                raise RuntimeError("Trace not complete in Observer: " + str(row.get("scenario","probe")))
        print(raw.strip(), flush=True)
    code, _ = probe_http(ports["observer"], "/")
    require(code==200,"Dashboard is unavailable")
    until = time.monotonic()+35
    while time.monotonic()<until:
        code, raw = probe_http(ports["observer"], "/api/v1/overview")
        if code == 200 and all((v or [0,0])[1] == 1 for v in json.loads(raw)["signals"].values()):
            print(encode({"healthSignals":"healthy","dashboard":True}))
            return
        time.sleep(1)
    raise RuntimeError("Prometheus signals not ready")


def verify_events(args):
    args.output.mkdir(parents=True, exist_ok=False)
    store = Store(args.output / "events.sqlite")
    keys = [(secrets.token_urlsafe(32), name+"-sim", name, True) for name in ("dag","agent","device")]
    with ObserverServer(("127.0.0.1",0), store, Sources("http://127.0.0.1:1","http://127.0.0.1:1"), keys) as observer:
        worker = threading.Thread(target=observer.serve_forever)
        worker.start()
        port = observer.server_port
        report = {"mode":"events-only","dashboard":False,"scenarios":{}}
        try:
            status, body = probe_http(port,"/")
            require(status == 200 and "研发监控".encode() in body, "Chinese dashboard HTML is unavailable")
            report["dashboard"] = True
            for (token_value, source, domain, _), builder in zip(keys, (dag,agent,device)):
                fixture = builder()
                endpoint = f"http://127.0.0.1:{port}"
                first = post(endpoint, "/ingest/v1/events", fixture.events, token_value)
                duplicate = post(endpoint, "/ingest/v1/events", fixture.events, token_value)
                require(first["inserted"] == len(fixture.events) and duplicate["inserted"] == 0, "Event deduplication failed")
                path = "/api/v1/events?domain=" + domain
                code, body = probe_http(port,path)
                body = json.loads(body)
                require(code == 200 and len(body["items"]) == len(fixture.events) and all(
                    x["source"] == source and x["simulated"] for x in body["items"]), "Query event identities mismatch")
                report["scenarios"][domain] = first["inserted"]
                if domain == "device":
                    require(body["commands"][0]["states"] == ["sent","ack","error","estop"], "Device ACK projection mismatch")
                else:
                    code, raw = probe_http(port, "/api/v1/traces/" + fixture.trace)
                    view = json.loads(raw)
                    require(code == 200 and view["status"] == "unavailable" and len(view["items"]) == first["inserted"],
                            "Trace source failure hid persisted events")
        finally:
            observer.shutdown()
            worker.join()
    (args.output / "report.json").write_text(encode(report) + "\n", encoding="utf-8")
    print(encode(report))


def verify_drain(args):
    import signal
    from types import SimpleNamespace
    sys.path.insert(0,str(ROOT/"benchmarks"))
    from phase15_business_benchmark import BusinessBenchmark, WebSocketClient
    from repro_runtime import owned_server_process
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    with disposable_database(args.postgres_prefix.resolve(), ROOT, out) as (connection, _):
        keyfile = out/"token.key"
        keyfile.touch(mode=0o600)
        keyfile.write_text(secrets.token_hex(32))
        env = {**clean_environment(),"WEB_PHASE11_REQUIRED":"1","WEB_FUNCTION3_ENABLE":"1",
               "WEB_PHASE11_DATABASE":connection,"WEB_PHASE11_TOKEN_KEY_FILE":str(keyfile),
               "WEB_PHASE11_FILE_ROOT":str(out/"files"),"WEB_FUNCTION3_MEDIA_ROOT":str(out/"media")}
        with owned_server_process(args.binary.resolve(),ROOT,env,out/"before.log") as (port,child):
            bench=BusinessBenchmark(SimpleNamespace(host="127.0.0.1",port=port,timeout=2))
            pair=bench.create_pair("obsdrain")
            client=WebSocketClient(pair.sender.api)
            client.connect()
            acknowledged = []
            entered = threading.Event()
            next_key = []

            def traffic():
                for i in range(100):
                    key="drain-"+str(i)+"-"+secrets.token_hex(5)
                    try:
                        client.send_json({"v":1,"type":"chat.send","id":key,"conversationId":str(pair.conversation),
                                          "content":"Graceful drain test"})
                        next_key[:] = [key]
                        if i==3:
                            entered.set()
                        data=client.wait("chat.accepted",key)
                        acknowledged.append((key,str(data["id"])))
                    except (AssertionError,OSError,EOFError):
                        return

            sender=threading.Thread(target=traffic)
            sender.start()
            try:
                require(entered.wait(8),"Could not start drain traffic")
                child.send_signal(signal.SIGTERM)
                require(child.wait(20)==0,"Graceful server exit returned nonzero")
            finally:
                sender.join(5)
                client.close()
            require(len(acknowledged)>=3 and not sender.is_alive(),"No verified ACKs before stop")
            proposed=next_key[0]
        with owned_server_process(args.binary.resolve(),ROOT,env,out/"after.log") as (port,_):
            pair.sender.api.close()
            pair.sender.api.port=port
            retry=WebSocketClient(pair.sender.api)
            retry.connect()
            try:
                first_ack = None
                for _ in range(2):
                    retry.send_json({"v":1,"type":"chat.send","id":proposed,"conversationId":str(pair.conversation),
                                     "content":"Graceful drain test"})
                    reply=retry.wait("chat.accepted",proposed)
                    if first_ack is None:
                        first_ack=str(reply["id"])
                    else:
                        require(reply["status"]=="duplicate" and str(reply["id"])==first_ack,"Restart retry duplicated a message")
                status, history=pair.sender.api.request("GET",f"/api/conversations/{pair.conversation}/messages?after=0&limit=200")
                history_ids=[str(row["id"]) for row in history]
                require(status==200 and all(history_ids.count(mid)==1 for _,mid in acknowledged)
                        and history_ids.count(first_ack)==1,"Durable ACKs are missing or duplicated")
            finally:
                retry.close()
                pair.sender.api.close()
                pair.receiver.api.close()
        result={"gracefulExit":True,"acknowledgedBeforeExit":len(acknowledged),"acknowledgedRowsRecovered":True,
                "lastAttemptIdempotentAfterRestart":True}
        print(encode(result))
        (out/"report.json").write_text(encode(result)+"\n",encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    wire = sub.add_parser("wire")
    for name in ("binary", "postgres-prefix", "output"):
        wire.add_argument("--"+name, type=Path, required=True)
    stack = sub.add_parser("stack")
    stack.add_argument("--run-dir", type=Path, required=True)
    events = sub.add_parser("events")
    events.add_argument("--output", type=Path, required=True)
    drain = sub.add_parser("drain")
    for name in ("binary", "postgres-prefix", "output"):
        drain.add_argument("--"+name, type=Path, required=True)
    args = parser.parse_args()
    {"wire":verify_wire,"stack":verify_stack,"events":verify_events,"drain":verify_drain}[args.mode](args)


if __name__ == "__main__":
    main()
