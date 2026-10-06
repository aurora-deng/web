#!/usr/bin/env python3

"""只用于集成测试的最小 Ollama HTTP 流服务，不提供任何真实推理能力。"""

import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    server_version = "Phase11FakeOllama/1.0"

    def log_message(self, _format: str, *_args: object) -> None:
        return

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        if self.path != "/api/chat":
            self.send_error(404)
            return
        size = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(size))
        if request.get("model") != "qwen-runtime" or request.get("stream") is not True:
            self.send_response(400)
            self.send_header("Content-Type", "application/x-ndjson")
            self.end_headers()
            self.wfile.write(b'{"error":"unexpected request"}\n')
            return

        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson")
        self.end_headers()
        for event in (
            {"message": {"content": "hello "}, "done": False},
            {"message": {"content": "from fake ollama"}, "done": False},
            {"done": True, "eval_count": 4},
        ):
            self.wfile.write(json.dumps(event, separators=(",", ":")).encode() + b"\n")
            self.wfile.flush()
            time.sleep(0.02)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"FAKE_OLLAMA_READY {args.port}", flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
