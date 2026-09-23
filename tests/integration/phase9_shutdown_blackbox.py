#!/usr/bin/env python3

import argparse
import base64
import os
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Tuple

HOST = "127.0.0.1"
PORT = 8080


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


def receive_until(sock: socket.socket, marker: bytes, initial: bytes = b"") -> Tuple[bytes, bytes]:
    data = bytearray(initial)
    while marker not in data:
        part = sock.recv(4096)
        if not part:
            raise AssertionError("connection closed before expected marker")
        data.extend(part)
    head, tail = bytes(data).split(marker, 1)
    return head + marker, tail


def read_chunk(sock: socket.socket, initial: bytes = b"") -> Tuple[str, bytes]:
    size_line, tail = receive_until(sock, b"\r\n", initial)
    size = int(size_line[:-2].split(b";", 1)[0], 16)
    needed = size + 2
    data = bytearray(tail)
    while len(data) < needed:
        part = sock.recv(4096)
        if not part:
            raise AssertionError("SSE closed in the middle of a chunk")
        data.extend(part)
    if data[size:size + 2] != b"\r\n":
        raise AssertionError("invalid chunk terminator")
    return bytes(data[:size]).decode(), bytes(data[needed:])


def stop(process: subprocess.Popen) -> None:
    os.killpg(process.pid, signal.SIGTERM)


def launch(server: Path, log) -> subprocess.Popen:
    process = subprocess.Popen(
        [str(server)], cwd=server.parent,
        env={**os.environ, "WEB_SERVER_REACTORS": "2", "WEB_GRPC_ADDRESS": "off"},
        stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
    )
    wait_ready(process)
    return process


def verify_sse_shutdown(server: Path, log) -> None:
    process = launch(server, log)
    sock = socket.create_connection((HOST, PORT), timeout=3)
    sock.settimeout(3)
    try:
        sock.sendall(
            b"GET /events?uid=7001 HTTP/1.1\r\n"
            b"Host: localhost\r\nAccept: text/event-stream\r\n\r\n"
        )
        header, tail = receive_until(sock, b"\r\n\r\n")
        if not header.startswith(b"HTTP/1.1 200 OK"):
            raise AssertionError(header)
        ready, tail = read_chunk(sock, tail)
        if "event: ready\n" not in ready:
            raise AssertionError(ready)

        stop(process)
        shutdown, _ = read_chunk(sock, tail)
        if shutdown != "event: server-shutdown\ndata: reconnect\n\n":
            raise AssertionError(("unexpected SSE shutdown event", shutdown))
        process.wait(timeout=3)
    finally:
        sock.close()
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)


def verify_websocket_shutdown(server: Path, log) -> None:
    process = launch(server, log)
    sock = socket.create_connection((HOST, PORT), timeout=3)
    sock.settimeout(3)
    try:
        key = base64.b64encode(b"phase9-ws-key-01").decode()
        sock.sendall(
            (
                "GET /ws?uid=7002 HTTP/1.1\r\n"
                "Host: localhost\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {key}\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n"
            ).encode()
        )
        header, tail = receive_until(sock, b"\r\n\r\n")
        if not header.startswith(b"HTTP/1.1 101 Switching Protocols"):
            raise AssertionError(header)

        stop(process)
        frame = bytearray(tail)
        while len(frame) < 4:
            frame.extend(sock.recv(4096))
        if frame[0] != 0x88:
            raise AssertionError(("expected WebSocket Close frame", bytes(frame)))
        length = frame[1] & 0x7F
        while len(frame) < 2 + length:
            frame.extend(sock.recv(4096))
        payload = bytes(frame[2:2 + length])
        if len(payload) < 2 or int.from_bytes(payload[:2], "big") != 1001:
            raise AssertionError(("expected close code 1001", payload))
        process.wait(timeout=3)
    finally:
        sock.close()
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, type=Path)
    server = parser.parse_args().server.resolve()
    with tempfile.TemporaryFile() as log:
        verify_sse_shutdown(server, log)
        verify_websocket_shutdown(server, log)
    print("all Phase 9 graceful-shutdown black-box checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
