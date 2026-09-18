#!/usr/bin/env python3
"""Linux smoke test: real TLS handshake, ALPN h2/h1, and HTTP/1.1 response."""

import argparse
import os
import shutil
import socket
import ssl
import subprocess
import tempfile
import time
from pathlib import Path


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_ready(process: subprocess.Popen, port: int) -> None:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited with {process.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise TimeoutError("TLS listener did not start")


def connect_tls(port: int, offered: list[str]) -> ssl.SSLSocket:
    # 自签名证书只用于本地冒烟测试；正式客户端必须校验证书及主机名。
    context = ssl._create_unverified_context()
    context.set_alpn_protocols(offered)
    raw = socket.create_connection(("127.0.0.1", port), timeout=5)
    try:
        return context.wrap_socket(raw, server_hostname="localhost")
    except Exception:
        raw.close()
        raise


def run(server: Path) -> None:
    if not shutil.which("openssl"):
        raise RuntimeError("openssl command is needed to generate a temporary test certificate")
    root = Path(__file__).resolve().parents[2]
    plain_port, tls_port = free_port(), free_port()
    while tls_port == plain_port:
        tls_port = free_port()
    with tempfile.TemporaryDirectory(prefix="web-tls-test-") as directory:
        cert = Path(directory) / "cert.pem"
        key = Path(directory) / "key.pem"
        log_path = Path(directory) / "server.log"
        subprocess.run(
            ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
             "-days", "1", "-keyout", str(key), "-out", str(cert),
             "-subj", "/CN=localhost"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            check=True, timeout=15,
        )
        env = os.environ.copy()
        env.update(WEB_SERVER_PORT=str(plain_port), WEB_TLS_PORT=str(tls_port),
                   WEB_TLS_CERT=str(cert), WEB_TLS_KEY=str(key), WEB_SERVER_REACTORS="1")
        with log_path.open("wb") as log:
            process = subprocess.Popen([str(server)], cwd=root, env=env,
                                       stdout=log, stderr=subprocess.STDOUT)
            try:
                wait_ready(process, tls_port)
                with connect_tls(tls_port, ["h2", "http/1.1"]) as sock:
                    assert sock.selected_alpn_protocol() == "h2"
                with connect_tls(tls_port, ["http/1.1"]) as sock:
                    assert sock.selected_alpn_protocol() == "http/1.1"
                    sock.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
                    assert sock.recv(4096).startswith(b"HTTP/1.1 200"), "TLS HTTP/1 response failed"
                # 有 HTTP/2 功能的 curl 才做真实 h2 请求；ALPN h2 检查始终执行。
                if shutil.which("curl") and "HTTP2" in subprocess.check_output(
                        ["curl", "--version"], text=True).upper():
                    result = subprocess.run(
                        ["curl", "-ksS", "--http2", "--max-time", "5", "-o", os.devnull,
                         "-w", "%{http_version} %{http_code}",
                         f"https://localhost:{tls_port}/"],
                        capture_output=True, text=True, timeout=8, check=True)
                    assert result.stdout.strip() == "2 200", result.stdout
                else:
                    print("curl HTTP2 unavailable: actual h2 request skipped; ALPN h2 checked")
                print("TLS/ALPN smoke PASS")
            except Exception:
                log.flush()
                print(log_path.read_text(errors="replace"))
                raise
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, type=Path)
    run(parser.parse_args().server.resolve())
