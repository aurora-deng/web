"""Bounded local receiver for the isolated Alertmanager confirmation exercise."""

import argparse
from collections import deque
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import re
import time


MAX_BODY = 64 * 1024
LABELS = {"alertname", "bc", "app", "instance", "job", "severity"}
VALUE = re.compile(r"[A-Za-z0-9_.:-]{1,128}\Z")


def project(body):
    if not isinstance(body, dict) or body.get("status") not in ("firing", "resolved"):
        raise ValueError("Invalid webhook status")
    alerts = body.get("alerts")
    if not isinstance(alerts, list) or not 1 <= len(alerts) <= 16:
        raise ValueError("Invalid webhook batch")
    result = []
    for row in alerts:
        if not isinstance(row, dict) or row.get("status") not in ("firing", "resolved") or not isinstance(row.get("labels"), dict):
            raise ValueError("Invalid alert")
        labels = {key: value for key, value in row["labels"].items()
                  if key in LABELS and isinstance(value, str) and VALUE.fullmatch(value)}
        if "alertname" not in labels:
            raise ValueError("Alert name is required")
        result.append({"status": row["status"], "labels": labels})
    return {"receivedAt": int(time.time()), "status": body["status"], "alerts": result}


class Receiver(HTTPServer):
    def __init__(self, port):
        super().__init__(("127.0.0.1", port), Handler)
        self.received = deque(maxlen=128)

    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(2)
        return connection, address


class Handler(BaseHTTPRequestHandler):
    def response(self, status, value):
        body = json.dumps(value).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/notifications":
            self.response(200, {"notifications": list(self.server.received)})
        elif self.path == "/health":
            self.response(200, {"status": "alive"})
        else:
            self.response(404, {"status": "not_found"})

    def do_POST(self):
        if self.path != "/webhook":
            self.response(404, {"status": "not_found"})
            return
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 1 <= size <= MAX_BODY:
                self.response(413, {"status": "too_large"})
                return
            record = project(json.loads(self.rfile.read(size)))
        except (OSError, ValueError):
            self.response(400, {"status": "invalid"})
            return
        self.server.received.append(record)
        self.response(200, {"status": "accepted"})

    def log_message(self, _format, *_args):
        pass


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--port", required=True, type=int)
    args = cli.parse_args()
    with Receiver(args.port) as server:
        server.serve_forever()


if __name__ == "__main__":
    main()
