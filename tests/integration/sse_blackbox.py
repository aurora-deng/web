#!/usr/bin/env python3

import argparse
import json
import os
import signal
import socket
import subprocess
import tempfile
import time
import urllib.parse
from pathlib import Path

HOST = "127.0.0.1"
PORT = 8080
TIMEOUT = 3.0


def wait_ready(process: subprocess.Popen) -> None:
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited with {process.returncode}")
        try:
            with socket.create_connection((HOST, PORT), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise TimeoutError("server did not listen on port 8080")


def receive_until(sock: socket.socket, marker: bytes, initial: bytes = b"") -> tuple[bytes, bytes]:
    data = bytearray(initial)
    while marker not in data:
        part = sock.recv(4096)
        if not part:
            raise AssertionError("connection closed before the expected delimiter")
        data.extend(part)
    head, tail = bytes(data).split(marker, 1)
    return head + marker, tail


class ChunkReader:
    def __init__(self, sock: socket.socket, initial: bytes = b"") -> None:
        self.sock = sock
        self.buffer = bytearray(initial)

    def _fill(self, size: int) -> None:
        while len(self.buffer) < size:
            part = self.sock.recv(4096)
            if not part:
                raise AssertionError("SSE connection closed while reading a chunk")
            self.buffer.extend(part)

    def read(self) -> str:
        while b"\r\n" not in self.buffer:
            self._fill(len(self.buffer) + 1)
        raw_size, remainder = bytes(self.buffer).split(b"\r\n", 1)
        self.buffer = bytearray(remainder)
        size = int(raw_size.split(b";", 1)[0], 16)
        if size == 0:
            raise AssertionError("SSE stream ended unexpectedly")
        self._fill(size + 2)
        payload = bytes(self.buffer[:size])
        if self.buffer[size : size + 2] != b"\r\n":
            raise AssertionError("invalid HTTP chunk terminator")
        del self.buffer[: size + 2]
        return payload.decode("utf-8")


def open_sse(uid: int) -> tuple[socket.socket, ChunkReader]:
    sock = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
    sock.settimeout(TIMEOUT)
    sock.sendall(
        (
            f"GET /events?uid={uid} HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Accept: text/event-stream\r\n"
            "Connection: keep-alive\r\n\r\n"
        ).encode()
    )
    header, tail = receive_until(sock, b"\r\n\r\n")
    lower = header.lower()
    if not header.startswith(b"HTTP/1.1 200 OK\r\n"):
        raise AssertionError(header)
    if b"content-type: text/event-stream; charset=utf-8\r\n" not in lower:
        raise AssertionError("missing SSE content type")
    if b"transfer-encoding: chunked\r\n" not in lower:
        raise AssertionError("missing chunked transfer encoding")
    reader = ChunkReader(sock, tail)
    ready = reader.read()
    if "event: ready\n" not in ready or "data: connected\n\n" not in ready:
        raise AssertionError(("invalid ready event", ready))
    return sock, reader


def request(path: str, method: str = "GET") -> tuple[bytes, bytes]:
    with socket.create_connection((HOST, PORT), timeout=TIMEOUT) as sock:
        sock.settimeout(TIMEOUT)
        sock.sendall(
            (
                f"{method} {path} HTTP/1.1\r\n"
                "Host: localhost\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n"
            ).encode()
        )
        data = bytearray()
        while True:
            part = sock.recv(4096)
            if not part:
                break
            data.extend(part)
    header, body = bytes(data).split(b"\r\n\r\n", 1)
    return header, body


def run_check() -> None:
    invalid_header, _ = request("/events")
    if not invalid_header.startswith(b"HTTP/1.1 400 Bad Request"):
        raise AssertionError(("missing uid was accepted", invalid_header))

    first_socket, first = open_sse(9101)
    second_socket, second = open_sse(9101)
    try:
        query = urllib.parse.urlencode(
            {"event": "notice", "id": "42", "data": "hello-SSE"}
        )
        header, body = request(f"/events/9101?{query}", method="POST")
        if not header.startswith(b"HTTP/1.1 200 OK"):
            raise AssertionError((header, body))
        result = json.loads(body)
        if result.get("acceptedConnections") != 2:
            raise AssertionError(("both tabs were not targeted", result))

        for reader in (first, second):
            event = reader.read()
            if event != "id: 42\nevent: notice\ndata: hello-SSE\n\n":
                raise AssertionError(("unexpected event", event))

        status_header, status_body = request("/events-status")
        if not status_header.startswith(b"HTTP/1.1 200 OK"):
            raise AssertionError(status_header)
        if json.loads(status_body).get("onlineConnections") != 2:
            raise AssertionError(("wrong online connection count", status_body))
    finally:
        first_socket.close()
        second_socket.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, type=Path)
    server = parser.parse_args().server.resolve()
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen(
            [str(server)],
            cwd=server.parent,
            env={**os.environ, "WEB_SERVER_REACTORS": "2"},
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            wait_ready(process)
            run_check()
            print("all SSE black-box checks passed")
            return 0
        finally:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)


if __name__ == "__main__":
    raise SystemExit(main())
