"""Replay the fleet edge's bounded JSONL spool into a local Observer."""
import argparse
import http.client
import io
import json
import os
from pathlib import Path
import re
from urllib.parse import urlsplit


def post_events(port, records, token):
    body = json.dumps(records, separators=(",", ":")).encode("utf-8")
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=4)
    try:
        connection.request("POST", "/ingest/v1/events", body, {
            "Authorization": "Bearer " + token,
            "Content-Type": "application/json",
        })
        with connection.getresponse() as response:
            if response.status != 200:
                raise RuntimeError(f"Observer rejected fleet events (HTTP {response.status})")
            if len(response.read(65537)) > 65536:
                raise RuntimeError("Observer response exceeds 64 KiB")
    finally:
        connection.close()


def local_observer_port(endpoint):
    url = urlsplit(endpoint)
    if url.scheme != "http" or not re.fullmatch(r"127\.0\.0\.1:[0-9]{1,5}", url.netloc) or (
            not url.port or url.path or url.query or url.fragment):
        raise ValueError("choose a local http://127.0.0.1:PORT Observer endpoint")
    return url.port


def producer_token():
    token = os.environ.get("NEBULA_FLEET_OBSERVER_TOKEN", "")
    if not token or len(token) > 256 or not all("!" <= char <= "~" for char in token):
        raise ValueError("set the authorized DAG producer token in NEBULA_FLEET_OBSERVER_TOKEN")
    return token


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("spool", type=Path)
    parser.add_argument("--observer", required=True)
    args = parser.parse_args()
    try:
        port, token = local_observer_port(args.observer), producer_token()
    except ValueError as error:
        parser.error(str(error))
    max_bytes = 8 * 1024 * 1024
    with args.spool.open("rb") as file:
        payload = file.read(max_bytes + 1)
    if len(payload) > max_bytes:
        parser.error("spool exceeds 8 MiB")
    records = []
    for line in io.BytesIO(payload):
        if not line.endswith(b"\n"):
            # The producer may still be appending the last event.
            continue
        if len(line) > 2048:
            parser.error("unexpected oversized event")
        event = json.loads(line)
        if not isinstance(event, dict) or event.get("eventType") not in ("dag.run", "dag.edge", "dag.node"):
            parser.error("the fleet spool only accepts DAG events")
        records.append(event)
    for at in range(0, len(records), 64):
        post_events(port, records[at:at + 64], token)
    print(json.dumps({"eventsSent": len(records), "traceCount": len({e["traceId"] for e in records})}))


if __name__ == "__main__":
    main()
