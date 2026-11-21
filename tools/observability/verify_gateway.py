"""Exercise the real ingress and its telemetry using only an owned gateway lab."""

import argparse
from contextlib import closing
import http.client
import json
import os
from pathlib import Path
import re
import secrets
import signal
import ssl
import sys
import time
from urllib.parse import urlencode

from .gateway import port_ok
from .observer.events import hex_id
from .observer.gateway_logs import valid_gateway_request_id
from .scenarios import read_observer


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts/project"))
from repro_runtime import browser_fixture, require


def owned_lab(directory):
    session = json.loads((directory / "session.json").read_text(encoding="utf-8"))
    ports = session["ports"]
    require(sys.platform.startswith("linux") and session["profile"] in ("metrics", "full") and "gateway" in ports,
            "Start an isolated Linux lab with the optional gateway")
    require(all(map(port_ok, (session["nebulaPort"], ports["gateway"], ports["observer"]))),
            "Protected daily services are excluded")
    owner = int((directory / "lab.pid").read_text())
    command = Path(f"/proc/{owner}/cmdline").read_bytes().split(b"\0")
    require(b"tools.observability.lab" in command and str(directory).encode() in command
            and b"--gateway-envoy" in command, "The isolated gateway owner is not running")
    children = Path(f"/proc/{owner}/task/{owner}/children").read_text().split()
    processes = {Path(f"/proc/{pid}/exe").resolve().name: int(pid) for pid in children}
    binary = Path(os.fsdecode(command[command.index(b"--gateway-envoy") + 1]))
    envoy = (Path(f"/proc/{owner}/cwd") / binary).resolve().name
    require({"webserver", envoy} <= processes.keys(), "Expected and owned gateway/upstream processes are missing")
    return session, processes["webserver"]


def exchange(client, method, path, body=None, headers=None, timeout=6):
    request_headers = {"Origin": client.origin, **(headers or {})}
    if client.cookie:
        request_headers["Cookie"] = client.cookie
    if client.csrf and method not in ("GET", "HEAD"):
        request_headers["X-CSRF-Token"] = client.csrf
    with closing(client.http_connection()) as connection:
        connection.timeout = timeout
        connection.request(method, path, body=body, headers=request_headers)
        response = connection.getresponse()
        return response.status, response.read(), {key.lower(): value for key, value in response.getheaders()}


def expect(client, method, path, payload=None, *, expected=200):
    status, value = client.request(method, path, payload)
    require(status == expected, f"{method} {path.split('?', 1)[0]} returned {status}, expected {expected}")
    return value


def event_for(reader, post_id, fixture):
    for _ in range(30):
        event = reader.read()
        if "event: tree.post.created\n" not in event:
            continue
        payload = fixture.sse_json(event)["payload"]
        if isinstance(payload, str):
            payload = json.loads(payload)
        if str(payload.get("postId")) == str(post_id):
            event_id = next(line[4:].strip() for line in event.splitlines() if line.startswith("id: "))
            require(re.fullmatch(r"(?:function3:)?[0-9]{1,19}", event_id), "Expected the replay event cursor")
            return event_id
    raise RuntimeError("The durable tree event did not arrive over SSE")


def verify_streams(author, peer, fixture, nonce):
    invite = expect(author, "POST", "/api/friend-requests", {"receiverId": str(peer.user_id)}, expected=201)
    expect(peer, "POST", f"/api/friend-requests/{invite['id']}", {"accept": "true"})
    conversation = expect(author, "POST", "/api/conversations",
                          {"kind": "direct", "peerId": str(peer.user_id)}, expected=201)["id"]
    command = {"v": 1, "type": "chat.send", "id": nonce, "conversationId": str(conversation),
               "content": "gateway-isolated-message"}

    blocked = fixture.BrowserClient(author.host, author.port, cookie=author.cookie,
                                    user_id=author.user_id, origin="https://example.invalid",
                                    tls_context=author.tls_context)
    try:
        denied_socket, _ = blocked.open_websocket()
    except AssertionError as error:
        require(" 403 " in str(error), "Cross-origin WebSocket did not return the expected 403")
    else:
        denied_socket.close()
        raise RuntimeError("Unexpected cross-origin WebSocket acceptance")

    def ack(expected):
        connection, reader = author.open_websocket()
        with closing(connection):
            connection.sendall(fixture.masked_frame(json.dumps(command).encode()))
            response = fixture.wait_for_ws(reader, "chat.accepted", attempts=12)
            require(response["status"] == expected, "Durable ACK status mismatch")

    ack("inserted")
    ack("duplicate")
    history = expect(peer, "GET", f"/api/conversations/{conversation}/messages?after=0&limit=20")
    require(len(history) == 1 and history[0]["clientMessageId"] == nonce, "Gateway retry changed message history")

    category = expect(author, "GET", "/api/tree/categories")[0]["id"]

    def publish(suffix):
        return expect(author, "POST", "/api/tree/posts",
                      {"requestId": nonce + suffix, "categoryId": category, "body": "gateway isolated story"},
                      expected=201)["id"]

    started = time.monotonic()
    events, reader = author.open_sse()
    first_event_ms = round((time.monotonic() - started) * 1000, 3)
    with closing(events):
        first_id = event_for(reader, publish("-first"), fixture)
    # Observe the second event on another connection before reconnecting the
    # first one. The replay assertion then requires Last-Event-ID to survive.
    events, reader = peer.open_sse()
    with closing(events):
        second_post = publish("-second")
        event_for(reader, second_post, fixture)
    events, reader = author.open_sse(last_event_id=first_id)
    with closing(events):
        event_for(reader, second_post, fixture)
    return first_event_ms


def verify_files(client):
    content = b"\x89PNG\r\n\x1a\n" + b"gateway-isolated-upload"
    meta = expect(client, "POST", "/api/files/uploads",
                  {"name": "gateway-check.png", "mediaType": "image/png", "size": str(len(content))}, expected=201)
    file_path = f"/api/files/{meta['id']}"
    code, _, _ = exchange(client, "PATCH", file_path + "/content", content,
                          {"Content-Type": "application/octet-stream", "Upload-Offset": "0"})
    require(code == 200, "File chunk not forwarded")
    require(expect(client, "POST", file_path + "/complete", {})["state"] == "ready", "Upload not completed")
    code, data, _ = exchange(client, "GET", file_path, headers={"Range": "bytes=4-15"})
    require(code == 206 and data == content[4:16], "Range response changed in the gateway")


def eventually(read, accept, failure, timeout=30):
    deadline = time.monotonic() + timeout
    while True:
        try:
            value = read()
            if accept(value):
                return value
        except (OSError, http.client.HTTPException):
            pass
        require(time.monotonic() < deadline, failure)
        time.sleep(.3)


def gateway_record(observer, request_id):
    query = urlencode({"source": "nebula-gateway", "q": request_id, "limit": 50})
    return eventually(lambda: read_observer(observer, "/api/v1/logs?" + query)["items"],
                      lambda rows: len(rows) == 1, "Correlated ingress access log did not appear")[0]


def verify_telemetry(client, observer, prometheus):
    private_canary = "omit-query-" + secrets.token_hex(8)
    status, _, headers = exchange(client, "GET", "/api/conversations?probe=" + private_canary)
    request_id = headers.get("x-request-id", "")
    require(status == 200 and valid_gateway_request_id(request_id), "Missing the gateway request identifier")
    row = gateway_record(observer, request_id)
    require(hex_id(row.get("traceId"), 32) and row["traceId"] == headers.get("x-nebula-trace-id")
            and row["status"] == row["upstreamStatus"] == 200
            and row["upstreamHeaderUs"] >= 0 and private_canary not in json.dumps(row),
            "Ingress request did not correlate with a sampled response")
    trace = eventually(lambda: read_observer(observer, "/api/v1/traces/" + row["traceId"]),
                       lambda detail: any(span["name"] == "PostgreSQL command" for span in detail["spans"]),
                       "Gateway log trace did not resolve to the application and database")
    expression = 'nebula_gateway_http_seconds_count{stage="request"}'
    query = urlencode({"query": expression})
    values = eventually(lambda: read_observer(prometheus, "/api/v1/query?" + query)["data"]["result"],
                        lambda samples: any(float(item["value"][1]) > 0 for item in samples),
                        "Prometheus has no gateway HTTP observations")
    result = {"requestId": request_id, "traceId": row["traceId"], "traceStatus": trace["status"],
              "spanCount": len(trace["spans"]), "countAtScrape": int(float(values[0]["value"][1]))}
    return result


def verify_gateway_scrape(prometheus, wasm_enabled):
    expected = ['up{job="gateway"}', 'sum(envoy_http_downstream_rq_total{job="gateway"})']
    if wasm_enabled:
        expected.extend(f'sum({{__name__=~".*nebula_{kind}_handshake_ms_count",job="gateway"}})'
                        for kind in ("sse", "websocket"))
    for expression in expected:
        query = urlencode({"query": expression})
        eventually(lambda: read_observer(prometheus, "/api/v1/query?" + query)["data"]["result"],
                   lambda rows: any(float(row["value"][1]) > 0 for row in rows),
                   "Prometheus did not collect the gateway or plugin metric")


def upstream_outage(client, server_pid, observer):
    os.kill(server_pid, signal.SIGSTOP)
    try:
        code, _, headers = exchange(client, "GET", "/api/conversations", timeout=19)
        require(code == 504, "Expected a gateway timeout during the owned upstream pause")
        ident = headers["x-request-id"]
        row = gateway_record(observer, ident)
        require(row["status"] == 504 and "upstreamStatus" not in row,
                "A gateway timeout was confused with an upstream HTTP error")
    finally:
        os.kill(server_pid, signal.SIGCONT)
    eventually(lambda: exchange(client, "GET", "/health/ready")[0],
               lambda status: status == 200, "The upstream did not recover")
    return {"outageHttp": 504, "recoveredHttp": 200, "requestId": ident}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ca", type=Path, help="Expected lab certificate when the gateway also serves HTTPS")
    parser.add_argument("--exercise-upstream-fault", action="store_true")
    args = parser.parse_args()
    session, server_pid = owned_lab(args.run_dir.resolve())
    require(session.get("httpTraceEvery") == 1, "Start this verification lab with --http-trace-every 1")
    ports = session["ports"]
    observer = f'http://127.0.0.1:{ports["observer"]}'
    prometheus = f'http://127.0.0.1:{ports["prometheus"]}'
    fixture = browser_fixture(ROOT)
    http = fixture.BrowserClient("127.0.0.1", ports["gateway"])
    require(exchange(http, "GET", "/health/ready")[0] == 200, "HTTP ingress is not ready")
    require(exchange(http, "GET", "/metrics")[0] == 403, "The gateway exposed the private scrape endpoint")
    tls_port = ports.get("gateway_https")
    require(bool(tls_port) == bool(args.ca), "Supply --ca when the lab gateway is configured for HTTPS")
    port = tls_port or ports["gateway"]
    scheme = "https" if tls_port else "http"
    context = ssl.create_default_context(cafile=str(args.ca.resolve())) if tls_port else None
    client_options = {"origin": f"{scheme}://127.0.0.1:{port}", "tls_context": context}
    author = fixture.BrowserClient("127.0.0.1", port, **client_options)
    peer = fixture.BrowserClient("127.0.0.1", port, **client_options)
    nonce = "gateway-" + secrets.token_hex(6)
    for path in ("/phase11/", "/phase11/tree/", "/phase11/admin/"):
        require(exchange(author, "GET", path)[0] == 200, "Gateway UI route failed")
    for index, client in enumerate((author, peer)):
        client.register_and_login(nonce.replace("-", "_") + str(index), secrets.token_urlsafe(24))
    first_event_ms = verify_streams(author, peer, fixture, nonce)
    verify_files(author)
    trace = verify_telemetry(author, observer, prometheus)
    verify_gateway_scrape(prometheus, session.get("gatewayWasm"))
    fault = upstream_outage(author, server_pid, observer) if args.exercise_upstream_fault else None
    report = {"passed": True, "http": True, "verifiedTls": context is not None,
              "websocketAckAndRetry": True, "deniedOrigin": True, "sseReplay": True,
              "sseReadyMs": first_event_ms, "uploadAndRange": True, "metricsPrivate": True,
              "envoyNativeScrape": True, "wasmScrape": True if session.get("gatewayWasm") else None,
              "gatewayTraceLookup": trace, "upstreamFault": fault}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
