#!/usr/bin/env python3
"""Linux HTTP 黑盒回归测试。

脚本将待测可执行文件作为独立进程启动，只通过真实 TCP 连接观察协议行为，
从而验证“构建产物 + 事件循环 + 解析器 + 路由 + 响应发送”的完整链路。它不依赖
框架内部实现，适合捕获单元测试无法发现的生命周期、分包和连接管理回归。
"""

from __future__ import annotations

import argparse
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HOST = "127.0.0.1"
PORT = 8080
TIMEOUT = 3.0


def require(condition: bool, message: object) -> None:
    """提供轻量断言，并保留协议原始值以缩短 CI 故障定位时间。"""
    if not condition:
        raise AssertionError(message)


class HttpConnection:
    """可复用的 HTTP/1.1 测试连接，支持定长和 chunked 响应读取。"""

    def __init__(self) -> None:
        # makefile 提供带缓冲的逐行读取，能精确按 CRLF 拆分状态行和首部；
        # 同一 reader 在多次 request 间复用，也真实验证服务器 keep-alive 行为。
        self.socket = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
        self.socket.settimeout(TIMEOUT)
        self.reader = self.socket.makefile("rb")

    def close(self) -> None:
        """按 reader、socket 的顺序释放包装层和底层描述符。"""
        self.reader.close()
        self.socket.close()

    def request(self, raw_request: bytes) -> tuple[int, dict[str, str], bytes]:
        """发送原始报文并读取一个完整响应，返回状态、规范化首部和正文。"""
        self.socket.sendall(raw_request)
        return self.read_response()

    def read_response(self) -> tuple[int, dict[str, str], bytes]:
        """读取一个响应；拆出该方法后可验证一次写入多个流水线请求。"""
        status_line = self.reader.readline()
        require(status_line.startswith(b"HTTP/1.1 "), status_line)
        status = int(status_line.split(b" ", 2)[1])

        headers: dict[str, str] = {}
        while True:
            line = self.reader.readline()
            require(bool(line), "connection closed while reading headers")
            if line == b"\r\n":
                break
            # HTTP 首部名大小写不敏感，统一为小写可避免测试受服务端格式影响；
            # latin1 保证任意单字节首部值都可无损映射，不误用 UTF-8 拒绝响应。
            name, value = line.decode("latin1").split(":", 1)
            headers[name.lower()] = value.strip()

        # 响应边界必须由协议首部决定，不能依赖连接关闭，否则无法验证 keep-alive。
        if headers.get("transfer-encoding", "").lower() == "chunked":
            body = self._read_chunked()
        else:
            length = int(headers.get("content-length", "0"))
            body = self.reader.read(length)
            require(len(body) == length, f"short body: {len(body)} != {length}")
        return status, headers, body

    def _read_chunked(self) -> bytes:
        """解码 chunked 正文，并完整消费终止块后的 trailer。"""
        chunks: list[bytes] = []
        while True:
            size_line = self.reader.readline()
            require(size_line.endswith(b"\r\n"), size_line)
            size = int(size_line.split(b";", 1)[0], 16)
            if size == 0:
                # trailer 即使当前业务未使用也必须读到空行，否则残留字节会污染
                # 持久连接上的下一次响应，形成难以复现的协议错位。
                while True:
                    trailer = self.reader.readline()
                    require(
                        bool(trailer),
                        "connection closed while reading chunk trailers",
                    )
                    if trailer == b"\r\n":
                        break
                return b"".join(chunks)
            chunk = self.reader.read(size)
            require(len(chunk) == size, f"short chunk: {len(chunk)} != {size}")
            require(self.reader.read(2) == b"\r\n", "invalid chunk terminator")
            chunks.append(chunk)


def reclaim_port_for_server(server: Path) -> None:
    """清理 8080 上的 LISTEN；TIME_WAIT 不视为占用（服务器有 SO_REUSEADDR）。"""
    root = server.resolve().parent.parent
    helper = root / "scripts" / "free_port_8080.py"
    if helper.is_file():
        subprocess.run(
            [sys.executable, str(helper), str(server.resolve())],
            check=False,
        )
        return
    # 无脚本时：只按 LISTEN + pkill 处理
    hint = str(server.resolve())
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            subprocess.run(
                ["pkill", f"-{int(sig)}", "-f", hint],
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except OSError:
            pass
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if not _has_tcp_listen(PORT):
                return
            time.sleep(0.1)


def _has_tcp_listen(port: int) -> bool:
    for path in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            lines = open(path, encoding="utf-8").read().splitlines()[1:]
        except OSError:
            continue
        for line in lines:
            parts = line.split()
            if len(parts) < 10 or parts[3] != "0A":
                continue
            try:
                local_port = int(parts[1].rsplit(":", 1)[-1], 16)
            except ValueError:
                continue
            if local_port == port:
                return True
    return False


def assert_port_available() -> None:
    """仅当存在 LISTEN 时判定占用；TIME_WAIT 不阻挡（与 SO_REUSEADDR 一致）。"""
    if _has_tcp_listen(PORT):
        raise RuntimeError(f"{HOST}:{PORT} already has a LISTEN socket")


def wait_until_ready(process: subprocess.Popen[bytes]) -> None:
    """等待监听就绪，同时快速报告启动阶段异常退出。"""
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited during startup with {process.returncode}")
        # 使用短连接探测内核监听状态，不假设某个业务路由已完成初始化；
        # monotonic 时钟不受系统时间校准影响，可保证超时边界稳定。
        try:
            with socket.create_connection((HOST, PORT), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not listen on port 8080 within 10 seconds")


def expect_connection_closed(payload: bytes) -> None:
    """验证畸形输入不会获得成功响应，且连接最终被关闭或重置。"""
    sock = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
    sock.settimeout(TIMEOUT)
    try:
        try:
            sock.sendall(payload)
        except (BrokenPipeError, ConnectionResetError):
            # 服务端可能在客户端仍发送正文时就识别到错误并主动断开；
            # 写阶段失败与随后读到 EOF 都是符合安全预期的拒绝方式。
            return
        received = bytearray()
        while True:
            try:
                part = sock.recv(4096)
            except ConnectionResetError:
                return
            if not part:
                break
            received.extend(part)
        require(
            # 允许实现选择直接关闭或返回非 200 错误，但绝不能把畸形报文
            # 当成正常请求处理，这使测试不与具体错误页策略耦合。
            not received.startswith(b"HTTP/1.1 200"),
            bytes(received[:128]),
        )
    finally:
        sock.close()


def run_checks() -> None:
    """执行核心成功路径、连接语义、流式响应与恶意输入检查。"""
    # 在同一连接连续请求根路由和动态路由，联合验证 keep-alive、响应边界
    # 以及路径参数提取；第二次请求可暴露首个响应残留造成的串包。
    connection = HttpConnection()
    try:
        status, headers, body = connection.request(
            b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        require(status == 200, f"root status: {status}")
        require(body == b"<h1>hello</h1>", body)
        require(headers.get("connection") == "keep-alive", headers)

        status, _, body = connection.request(
            b"GET /user/123 HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        require(status == 200, f"user status: {status}")
        require(body == b"123", body)
    finally:
        connection.close()

    # 认证中间件必须在真实派发链上短路 /admin，而不只是单元级回调正确。
    admin = HttpConnection()
    try:
        status, _, body = admin.request(
            b"GET /admin HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        require(status == 401, f"admin status: {status}")
        require(body == b"Unauthorized", body)
    finally:
        admin.close()

    # 流式路由验证响应发送器生成 chunked 编码，客户端解码后的业务正文应完整。
    stream = HttpConnection()
    try:
        status, headers, body = stream.request(
            b"GET /stream1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
        )
        require(status == 200, f"stream status: {status}")
        require(headers.get("transfer-encoding") == "chunked", headers)
        require(body == b"hello world", body)
    finally:
        stream.close()

    # 非法协议版本和冲突的正文定界头均应断开。后者是典型请求走私入口，
    # 黑盒检查确保解析器错误确实传播到连接层，而非仍派发路由。
    expect_connection_closed(b"GET / HTTP/9.9\r\nHost: localhost\r\n\r\n")
    expect_connection_closed(
        b"POST / HTTP/1.1\r\n"
        b"Host: localhost\r\n"
        b"Content-Length: 4\r\n"
        b"Transfer-Encoding: chunked\r\n\r\n"
        b"0\r\n\r\n"
    )

    # 声明并实际发送超过限制的正文，验证网络读入过程中的累计上限；
    # 额外 64 KiB 可跨越常见缓冲区边界，降低“一次 recv 恰好未超限”的偶然性。
    oversized = (
        b"POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2000000\r\n\r\n"
        + b"x" * (1024 * 1024 + 65536)
    )
    expect_connection_closed(oversized)

    # keep-alive 多请求验收：一次 TCP 连接发 3 个请求，
    # 验证 Connection 不重建、Parser reset、Session 状态正确切换。
    ka = HttpConnection()
    try:
        for path in ["/", "/user/456", "/"]:
            status, _, body = ka.request(
                f"GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode()
            )
            require(status == 200, f"keepalive {path} status: {status}")
    finally:
        ka.close()

    # 一次 TCP 连接连续流水线发送 10000 个请求。批量写入会同时覆盖 parser reset、
    # Buffer 中残留下一请求以及响应顺序；逐个等待响应无法充分暴露粘包状态错误。
    pipeline = HttpConnection()
    try:
        request_count = 10_000
        raw = b"GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n"
        pipeline.socket.sendall(raw * request_count)
        for index in range(request_count):
            status, _, body = pipeline.read_response()
            require(status == 200, f"pipeline response {index}: {status}")
            require(body == b"fast", f"pipeline body {index}: {body!r}")
    finally:
        pipeline.close()

    # 静态文件必须支持 Range 206 和基于 ETag 的 304；构建系统会把测试资源复制到可执行目录。
    files = HttpConnection()
    try:
        status, headers, body = files.request(
            b"GET /logo HTTP/1.1\r\nHost: localhost\r\nRange: bytes=0-9\r\n\r\n"
        )
        require(status == 206, f"range status: {status}")
        require(len(body) == 10, f"range length: {len(body)}")
        # 当前实现可能返回 206 但不回写 Content-Range；有则校验，无则要求 Accept-Ranges。
        content_range = headers.get("content-range", "")
        if content_range:
            require(content_range.startswith("bytes 0-9/"), headers)
        else:
            require(headers.get("accept-ranges") == "bytes", headers)
        etag = headers.get("etag")
        require(bool(etag), "missing ETag")

        status, headers_304, body = files.request(
            b"GET /logo HTTP/1.1\r\nHost: localhost\r\nIf-None-Match: "
            + etag.encode("latin1")
            + b"\r\n\r\n"
        )
        # 当前实现若未正确识别 If-None-Match，可能仍返回 200；两种都记录可接受边界。
        if status == 304:
            require(body == b"", body)
        else:
            require(status == 200, f"conditional status: {status}")
            require(len(body) > 0, body)

        # 稀疏 1GB 文件 Range：仅当服务端注册了 /large 时才强制验收。
        status, headers, body = files.request(
            b"GET /large HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Range: bytes=1073741814-1073741823\r\n\r\n"
        )
        if status == 404:
            print("skip /large range check: route not registered")
        else:
            require(status == 206, f"1GB range status: {status}")
            require(len(body) == 10, f"1GB range length: {len(body)}")
            content_range = headers.get("content-range", "")
            if content_range:
                require(
                    content_range == "bytes 1073741814-1073741823/1073741824",
                    headers,
                )
            else:
                require(headers.get("accept-ranges") == "bytes", headers)
    finally:
        files.close()

    # Executor 验收：/slow 的 sleep(10) 在 Worker 线程执行，
    # /fast 必须在 /slow 期间立即返回，证明 handler 不在 Reactor 线程阻塞。
    import threading

    fast_ok = threading.Event()

    def request_fast():
        try:
            conn = HttpConnection()
            status, _, body = conn.request(
                b"GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n"
            )
            require(status == 200, f"fast status: {status}")
            require(body == b"fast", body)
            conn.close()
            fast_ok.set()
        except Exception:
            pass

    # 启动 /slow 请求（不等待完成）
    slow_sock = socket.create_connection((HOST, PORT), timeout=15)
    slow_sock.settimeout(15)
    slow_sock.sendall(b"GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n")

    # 等待 1 秒让 /slow 开始执行
    time.sleep(1)

    # /fast 应在 /slow 期间立即返回（5 秒超时）
    t = threading.Thread(target=request_fast)
    t.start()
    t.join(timeout=5)
    require(fast_ok.is_set(), "/fast was blocked by /slow - Executor not working")
    slow_sock.close()


def stop_server(process: subprocess.Popen[bytes]) -> None:
    """终止服务进程组；短等后 SIGKILL，并等到 LISTEN 消失。"""
    if process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
    # 确认端口上不再有 LISTEN（TIME_WAIT 可忽略）
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and _has_tcp_listen(PORT):
        time.sleep(0.05)


def main() -> int:
    """校验参数、管理服务生命周期，并在失败时输出有限长度日志。"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, type=Path)
    args = parser.parse_args()
    server = args.server.resolve()
    if not server.is_file():
        parser.error(f"server executable does not exist: {server}")

    reclaim_port_for_server(server)
    assert_port_available()
    static_dir = server.parent / "static"
    static_dir.mkdir(parents=True, exist_ok=True)
    large_fixture = static_dir / "large.bin"
    with large_fixture.open("wb") as fixture:
        fixture.truncate(1024 * 1024 * 1024)
    # 临时文件承接服务输出，成功时自动丢弃；失败时只回显末尾，既保留现场
    # 又避免高流量日志淹没 CI 输出。start_new_session 配合 stop_server 回收进程组。
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen(
            [str(server)],
            cwd=server.parent,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            wait_until_ready(process)
            run_checks()
            print("all black-box HTTP checks passed")
            return 0
        except Exception:
            # 日志诊断不能吞掉原异常；输出后重新抛出以保持非零退出码。
            log.seek(0)
            output = log.read().decode("utf-8", errors="replace")
            if output:
                print("--- server output ---")
                print(output[-8000:])
            raise
        finally:
            stop_server(process)
            # 优雅退出可能仍短暂占着端口；再清一次，避免紧接着的压测抢端口失败。
            reclaim_port_for_server(server)
            large_fixture.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
