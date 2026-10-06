#!/usr/bin/env python3

import argparse
import hashlib
import hmac
import os
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Dict, Optional, Tuple

HOST = "127.0.0.1"
PORT = int(os.environ.get("WEB_TEST_PORT", "8080"))
SECRET = "phase9-blackbox-auth-secret-at-least-32-bytes"
ORIGIN = "https://learning.example"


def token(user_id: int, tenant: str, expiry: Optional[int] = None) -> str:
    expiry = expiry or int(time.time()) + 3600
    payload = f"v1.{user_id}.{expiry}.{tenant}"
    signature = hmac.new(SECRET.encode(), payload.encode(), hashlib.sha256).hexdigest()
    return f"{payload}.{signature}"


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


def request(path: str, headers: Optional[Dict[str, str]] = None,
            method: str = "GET") -> Tuple[bytes, bytes]:
    values = headers or {}
    with socket.create_connection((HOST, PORT), timeout=3) as sock:
        sock.settimeout(3)
        lines = [f"{method} {path} HTTP/1.1", "Host: localhost",
                 "Content-Length: 0", "Connection: close"]
        lines.extend(f"{name}: {value}" for name, value in values.items())
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        data = bytearray()
        while b"\r\n\r\n" not in data:
            part = sock.recv(4096)
            if not part:
                break
            data.extend(part)
        head, _, body = bytes(data).partition(b"\r\n\r\n")
        content_length = 0
        for line in head.split(b"\r\n")[1:]:
            name, separator, value = line.partition(b":")
            if separator and name.lower() == b"content-length":
                content_length = int(value.strip())
                break
        while len(body) < content_length:
            part = sock.recv(4096)
            if not part:
                break
            body += part
        return head, body


def expect_status(header: bytes, status: int) -> None:
    expected = f"HTTP/1.1 {status} ".encode()
    if not header.startswith(expected):
        raise AssertionError((expected, header))


def verify_fail_closed_configuration(server: Path) -> None:
    env = dict(os.environ)
    env.update({"WEB_PRODUCTION_MODE": "1", "WEB_GRPC_ADDRESS": "off"})
    for name in ("WEB_AUTH_SECRET", "WEB_ALLOWED_ORIGINS", "WEB_TLS_CERT", "WEB_TLS_KEY"):
        env.pop(name, None)
    result = subprocess.run(
        [str(server)], cwd=server.parent, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=5, check=False
    )
    if result.returncode == 0 or b"WEB_AUTH_SECRET" not in result.stdout:
        raise AssertionError(("production configuration did not fail closed", result.stdout))


def run_checks() -> None:
    learning = token(42, "learning")
    operations = token(9000, "ops")

    header, _ = request("/health/live")
    expect_status(header, 200)

    header, _ = request("/events?uid=42", {"Origin": ORIGIN})
    expect_status(header, 401)

    header, _ = request(
        "/events?uid=43",
        {"Authorization": "Bearer " + learning, "Origin": ORIGIN},
    )
    expect_status(header, 403)

    header, _ = request(
        "/ws?uid=42",
        {"Authorization": "Bearer " + learning, "Origin": "https://evil.example"},
    )
    expect_status(header, 403)

    header, _ = request(
        "/metrics", {"Authorization": "Bearer " + learning}
    )
    expect_status(header, 403)

    header, body = request(
        "/metrics", {"Authorization": "Bearer " + operations}
    )
    expect_status(header, 200)
    combined = header + b"\r\n\r\n" + body
    if b"text/plain; version=0.0.4" not in header.lower() or \
       b"webserver_auth_rejected_total" not in combined:
        raise AssertionError("Prometheus metrics endpoint is incomplete")

    # Cookie 认证的写请求必须同时通过 Origin，阻断浏览器 CSRF；Bearer 服务调用
    # 不强制伪造一个浏览器 Origin。
    header, _ = request(
        "/events/42?data=csrf",
        {"Cookie": "web_session=" + operations}, method="POST"
    )
    expect_status(header, 403)
    header, _ = request(
        "/events/42?data=trusted",
        {"Cookie": "web_session=" + operations, "Origin": ORIGIN}, method="POST"
    )
    expect_status(header, 404)

    bad = learning[:-1] + ("0" if learning[-1] != "0" else "1")
    header, _ = request(
        "/events", {"Cookie": "web_session=" + bad, "Origin": ORIGIN}
    )
    expect_status(header, 401)

    # EventSource 无法自定义 Authorization，但浏览器会发送同源 Cookie；
    # 不再需要 uid 查询参数，服务端从签名身份中绑定 clientId。
    header, _ = request(
        "/events", {"Cookie": "web_session=" + learning, "Origin": ORIGIN}
    )
    expect_status(header, 200)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, type=Path)
    server = parser.parse_args().server.resolve()
    verify_fail_closed_configuration(server)

    with tempfile.TemporaryFile() as log:
        env = dict(os.environ)
        env.update({
            "WEB_SERVER_REACTORS": "2",
            "WEB_SERVER_PORT": str(PORT),
            "WEB_AUTH_SECRET": SECRET,
            "WEB_ALLOWED_ORIGINS": ORIGIN,
            "WEB_GRPC_ADDRESS": "off",
        })
        process = subprocess.Popen(
            [str(server)], cwd=server.parent, env=env,
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
        )
        try:
            wait_ready(process)
            run_checks()
            print("all Phase 9 security black-box checks passed")
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
