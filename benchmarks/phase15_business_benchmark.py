#!/usr/bin/env python3

"""Phase 15 真实业务基准。

只使用 Python 标准库，从网络入口驱动真实 HTTP、WebSocket、SSE、PostgreSQL
和 Outbox 路径。脚本不会直接写数据库，也不会把 /fast 的结果冒充业务性能。
"""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import hashlib
import http.client
import json
import math
import os
import socket
import statistics
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional


def percentile(values: list[float], wanted: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * wanted
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


@dataclass
class Metric:
    name: str
    latencies_ms: list[float] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)
    error_count: int = 0
    started: float = 0.0
    elapsed: float = 0.0
    units: int = 0
    extra: dict = field(default_factory=dict)

    def begin(self) -> None:
        self.started = time.perf_counter()

    def finish(self) -> None:
        self.elapsed = max(1e-9, time.perf_counter() - self.started)

    def observe(self, started: float, units: int = 1) -> None:
        self.latencies_ms.append((time.perf_counter() - started) * 1000.0)
        self.units += units

    def failure(self, error: BaseException) -> None:
        self.error_count += 1
        if len(self.errors) < 20:
            self.errors.append(f"{type(error).__name__}: {error}")

    def summary(self) -> dict:
        values = self.latencies_ms
        return {
            "name": self.name,
            "operations": len(values),
            "units": self.units,
            "elapsedSeconds": round(self.elapsed, 6),
            "operationsPerSecond": round(len(values) / self.elapsed, 3),
            "unitsPerSecond": round(self.units / self.elapsed, 3),
            "latencyMs": {
                "average": round(statistics.fmean(values), 3) if values else 0.0,
                "p50": round(percentile(values, 0.50), 3),
                "p95": round(percentile(values, 0.95), 3),
                "p99": round(percentile(values, 0.99), 3),
                "maximum": round(max(values), 3) if values else 0.0,
            },
            "errors": self.error_count,
            "errorSamples": self.errors,
            **self.extra,
        }


class ResourceSampler:
    """从 /proc 读取服务进程资源；不可用时留下 NA，而不是编造 0。"""

    def __init__(self, pid: int) -> None:
        self.pid = pid
        self.samples: list[tuple[float, int, int, int, float]] = []
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None

    def start(self) -> None:
        if self.pid <= 0 or not Path(f"/proc/{self.pid}").exists():
            return
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self) -> dict:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2.0)
        if not self.samples:
            return {"available": False}
        rss = [item[1] for item in self.samples]
        fds = [item[2] for item in self.samples]
        threads = [item[3] for item in self.samples]
        cpu = [item[4] for item in self.samples if item[4] >= 0]
        return {
            "available": True,
            "samples": len(self.samples),
            "rssKiB": {"average": round(statistics.fmean(rss), 1), "peak": max(rss)},
            "fileDescriptors": {"average": round(statistics.fmean(fds), 1), "peak": max(fds)},
            "threads": {"average": round(statistics.fmean(threads), 1), "peak": max(threads)},
            "cpuPercent": {
                "average": round(statistics.fmean(cpu), 2) if cpu else None,
                "peak": round(max(cpu), 2) if cpu else None,
            },
        }

    @staticmethod
    def read_process(pid: int) -> tuple[int, int, int, int]:
        """读取一个时刻的 RSS/FD/线程/CPU tick，供采样器和容量阶梯共用。"""
        status = Path(f"/proc/{pid}/status").read_text()
        rss = int(next(line.split()[1] for line in status.splitlines()
                       if line.startswith("VmRSS:")))
        threads = int(next(line.split()[1] for line in status.splitlines()
                           if line.startswith("Threads:")))
        fds = len(list(Path(f"/proc/{pid}/fd").iterdir()))
        fields = Path(f"/proc/{pid}/stat").read_text().split()
        ticks = int(fields[13]) + int(fields[14])
        return rss, fds, threads, ticks

    def snapshot(self) -> dict:
        """返回可序列化的进程快照；没有 /proc 时明确标记不可用。"""
        if self.pid <= 0 or not Path(f"/proc/{self.pid}").exists():
            return {"available": False}
        try:
            rss, fds, threads, _ticks = self.read_process(self.pid)
            return {
                "available": True,
                "rssKiB": rss,
                "fileDescriptors": fds,
                "threads": threads,
            }
        except (OSError, StopIteration, ValueError):
            return {"available": False}

    def _run(self) -> None:
        clock_ticks = os.sysconf("SC_CLK_TCK")
        previous_time = time.monotonic()
        previous_ticks: Optional[int] = None
        while not self._stop.wait(0.2):
            try:
                rss, fds, threads, ticks = self.read_process(self.pid)
                current = time.monotonic()
                cpu = -1.0
                if previous_ticks is not None and current > previous_time:
                    cpu = ((ticks - previous_ticks) / clock_ticks) / \
                          (current - previous_time) * 100.0
                previous_ticks = ticks
                previous_time = current
                self.samples.append((time.time(), rss, fds, threads, cpu))
            except (OSError, StopIteration, ValueError):
                return


class ApiClient:
    def __init__(self, host: str, port: int, timeout: float) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self.cookie = ""
        self.csrf = ""
        self.user_id = 0
        self.connection: Optional[http.client.HTTPConnection] = None

    def close(self) -> None:
        if self.connection:
            self.connection.close()
            self.connection = None

    def request(self, method: str, path: str,
                body: Optional[dict] = None) -> tuple[int, object]:
        encoded = b""
        headers = {"Accept": "application/json"}
        if body is not None:
            encoded = json.dumps(body, separators=(",", ":")).encode()
            headers["Content-Type"] = "application/json"
        if self.cookie:
            headers["Cookie"] = self.cookie
        if method not in ("GET", "HEAD") and self.csrf:
            headers["X-CSRF-Token"] = self.csrf
            headers["Origin"] = "http://localhost"
        for attempt in range(2):
            try:
                if self.connection is None:
                    self.connection = http.client.HTTPConnection(
                        self.host, self.port, timeout=self.timeout)
                self.connection.request(method, path, encoded, headers)
                response = self.connection.getresponse()
                raw = response.read()
                cookie = response.getheader("Set-Cookie")
                if cookie:
                    self.cookie = cookie.split(";", 1)[0]
                if response.getheader("Connection", "").lower() == "close":
                    self.close()
                parsed: object = json.loads(raw) if raw else {}
                return response.status, parsed
            except (ConnectionError, OSError, http.client.HTTPException):
                self.close()
                if attempt:
                    raise
        raise AssertionError("unreachable")

    def register(self, username: str, password: str) -> None:
        status, data = self.request(
            "POST", "/api/auth/register",
            {"username": username, "password": password, "displayName": username},
        )
        if status != 201 or not isinstance(data, dict):
            raise AssertionError(("register", status, data))
        self.user_id = int(data["user"]["id"])

    def login(self, username: str, password: str) -> None:
        status, data = self.request(
            "POST", "/api/auth/login", {"username": username, "password": password}
        )
        if status != 200 or not isinstance(data, dict):
            raise AssertionError(("login", status, data))
        self.user_id = int(data["user"]["id"])
        self.csrf = str(data["csrfToken"])


def masked_frame(payload: bytes, opcode: int = 0x1) -> bytes:
    mask = os.urandom(4)
    encoded = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    size = len(payload)
    if size < 126:
        length = bytes((0x80 | size,))
    elif size <= 0xFFFF:
        length = bytes((0x80 | 126,)) + size.to_bytes(2, "big")
    else:
        length = bytes((0x80 | 127,)) + size.to_bytes(8, "big")
    return bytes((0x80 | opcode,)) + length + mask + encoded


class WebSocketClient:
    def __init__(self, api: ApiClient) -> None:
        self.api = api
        self.socket: Optional[socket.socket] = None
        self.buffer = bytearray()

    def connect(self) -> None:
        key = base64.b64encode(os.urandom(16)).decode()
        expected = base64.b64encode(hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()
        ).digest())
        sock = socket.create_connection(
            (self.api.host, self.api.port), timeout=self.api.timeout)
        sock.settimeout(self.api.timeout)
        sock.sendall((
            "GET /ws HTTP/1.1\r\n"
            f"Host: {self.api.host}:{self.api.port}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
            "Origin: http://localhost\r\n"
            f"Cookie: {self.api.cookie}\r\n\r\n"
        ).encode())
        headers = bytearray()
        while b"\r\n\r\n" not in headers:
            part = sock.recv(4096)
            if not part:
                raise AssertionError("WebSocket handshake closed")
            headers.extend(part)
        raw, tail = bytes(headers).split(b"\r\n\r\n", 1)
        if not raw.startswith(b"HTTP/1.1 101") or expected not in raw:
            raise AssertionError(("WebSocket handshake", raw[:300]))
        self.socket = sock
        self.buffer.extend(tail)

    def close(self) -> None:
        if self.socket:
            try:
                self.socket.sendall(masked_frame(b"", 0x8))
            except OSError:
                pass
            self.socket.close()
            self.socket = None

    def send_json(self, data: dict) -> None:
        assert self.socket
        self.socket.sendall(masked_frame(
            json.dumps(data, separators=(",", ":")).encode()))

    def ping(self, payload: bytes = b"capacity") -> None:
        """写入轻量协议 Ping，确认驻留连接仍可写且让服务端走控制帧路径。"""
        assert self.socket
        self.socket.sendall(masked_frame(payload, 0x9))

    def _exact(self, size: int) -> bytes:
        assert self.socket
        while len(self.buffer) < size:
            part = self.socket.recv(4096)
            if not part:
                raise AssertionError("WebSocket closed")
            self.buffer.extend(part)
        result = bytes(self.buffer[:size])
        del self.buffer[:size]
        return result

    def read_json(self) -> dict:
        while True:
            header = self._exact(2)
            opcode = header[0] & 0x0F
            length = header[1] & 0x7F
            if length == 126:
                length = int.from_bytes(self._exact(2), "big")
            elif length == 127:
                length = int.from_bytes(self._exact(8), "big")
            payload = self._exact(length)
            if opcode == 0x9:
                assert self.socket
                self.socket.sendall(masked_frame(payload, 0xA))
                continue
            if opcode == 0xA:
                continue
            if opcode == 0x8:
                raise AssertionError("WebSocket close while waiting for event")
            if opcode != 0x1:
                continue
            return json.loads(payload)

    def wait(self, event_type: str, reply_to: str = "",
             client_message_id: str = "", attempts: int = 100) -> dict:
        for _ in range(attempts):
            message = self.read_json()
            if message.get("type") != event_type:
                continue
            if reply_to and message.get("replyTo") != reply_to:
                continue
            if client_message_id and \
                    message.get("clientMessageId") != client_message_id:
                continue
            return message
        raise AssertionError(f"missing {event_type} replyTo={reply_to}")


class SseClient:
    def __init__(self, api: ApiClient) -> None:
        self.api = api
        self.socket: Optional[socket.socket] = None
        self.buffer = bytearray()

    def connect(self) -> None:
        sock = socket.create_connection(
            (self.api.host, self.api.port), timeout=self.api.timeout)
        sock.settimeout(self.api.timeout)
        sock.sendall((
            "GET /events HTTP/1.1\r\n"
            f"Host: {self.api.host}:{self.api.port}\r\n"
            "Accept: text/event-stream\r\nConnection: keep-alive\r\n"
            "Origin: http://localhost\r\n"
            f"Cookie: {self.api.cookie}\r\n\r\n"
        ).encode())
        headers = bytearray()
        while b"\r\n\r\n" not in headers:
            part = sock.recv(4096)
            if not part:
                raise AssertionError("SSE handshake closed")
            headers.extend(part)
        raw, tail = bytes(headers).split(b"\r\n\r\n", 1)
        if not raw.startswith(b"HTTP/1.1 200"):
            raise AssertionError(("SSE handshake", raw[:300]))
        self.socket = sock
        self.buffer.extend(tail)
        ready = self.read_event()
        if ready[0] != "ready":
            raise AssertionError(("SSE ready", ready))

    def close(self) -> None:
        if self.socket:
            self.socket.close()
            self.socket = None

    def _fill(self, size: int) -> None:
        assert self.socket
        while len(self.buffer) < size:
            part = self.socket.recv(4096)
            if not part:
                raise AssertionError("SSE closed")
            self.buffer.extend(part)

    def read_event(self) -> tuple[str, object]:
        while b"\r\n" not in self.buffer:
            self._fill(len(self.buffer) + 1)
        raw_size, rest = bytes(self.buffer).split(b"\r\n", 1)
        self.buffer = bytearray(rest)
        size = int(raw_size.split(b";", 1)[0], 16)
        self._fill(size + 2)
        payload = bytes(self.buffer[:size]).decode()
        del self.buffer[:size + 2]
        event = next((line[7:] for line in payload.splitlines()
                      if line.startswith("event: ")), "message")
        data = "\n".join(line[6:] for line in payload.splitlines()
                          if line.startswith("data: "))
        if not data:
            return event, {}
        try:
            return event, json.loads(data)
        except json.JSONDecodeError:
            # SSE 允许 data 是任意 UTF-8 文本；ready 事件就是 connected。
            return event, data


@dataclass
class UserFixture:
    username: str
    password: str
    api: ApiClient


@dataclass
class PairFixture:
    sender: UserFixture
    receiver: UserFixture
    conversation: int


@dataclass
class AiFixture:
    actor: UserFixture
    conversation: int
    websocket: WebSocketClient
    events: SseClient


class BusinessBenchmark:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.suffix = str(time.time_ns())[-9:]
        self.users: list[UserFixture] = []
        self.users_lock = threading.Lock()
        self.pairs: list[PairFixture] = []
        self.results: list[dict] = []

    def application_metrics(self) -> dict[str, float]:
        connection = http.client.HTTPConnection(
            self.args.host, self.args.port, timeout=self.args.timeout)
        try:
            connection.request("GET", "/metrics", headers={"Accept": "text/plain"})
            response = connection.getresponse()
            body = response.read().decode("utf-8", "replace")
            if response.status != 200:
                raise AssertionError(("metrics", response.status, body[:200]))
        finally:
            connection.close()
        values: dict[str, float] = {}
        for line in body.splitlines():
            if not line.startswith("webserver_phase11_"):
                continue
            fields = line.split()
            if len(fields) == 2:
                try:
                    values[fields[0]] = float(fields[1])
                except ValueError:
                    pass
        return values

    def create_user(self, label: str) -> UserFixture:
        username = f"p15_{label}_{self.suffix}"[:32]
        password = "phase15-correct-password"
        api = ApiClient(self.args.host, self.args.port, self.args.timeout)
        api.register(username, password)
        api.login(username, password)
        fixture = UserFixture(username, password, api)
        with self.users_lock:
            self.users.append(fixture)
        return fixture

    def make_friends(self, first: UserFixture, second: UserFixture) -> None:
        status, request = first.api.request(
            "POST", "/api/friend-requests", {"receiverId": str(second.api.user_id)})
        if status != 201 or not isinstance(request, dict):
            raise AssertionError(("friend request", status, request))
        status, accepted = second.api.request(
            "POST", f"/api/friend-requests/{request['id']}", {"accept": "true"})
        if status != 200:
            raise AssertionError(("friend accept", status, accepted))

    def create_pair(self, label: str) -> PairFixture:
        sender = self.create_user(label + "a")
        receiver = self.create_user(label + "b")
        self.make_friends(sender, receiver)
        status, conversation = sender.api.request(
            "POST", "/api/conversations",
            {"kind": "direct", "peerId": str(receiver.api.user_id)})
        if status != 201 or not isinstance(conversation, dict):
            raise AssertionError(("direct conversation", status, conversation))
        pair = PairFixture(sender, receiver, int(conversation["id"]))
        self.pairs.append(pair)
        return pair

    def run_parallel(self, name: str, workers: int, operations: int,
                     operation: Callable[[int, int], None], units: int = 1) -> Metric:
        metric = Metric(name)
        lock = threading.Lock()
        next_index = 0

        def worker(worker_id: int) -> None:
            nonlocal next_index
            while True:
                with lock:
                    if next_index >= operations:
                        return
                    index = next_index
                    next_index += 1
                started = time.perf_counter()
                try:
                    operation(worker_id, index)
                    with lock:
                        metric.observe(started, units)
                except BaseException as error:
                    with lock:
                        metric.failure(error)

        metric.begin()
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            list(pool.map(worker, range(workers)))
        metric.finish()
        return metric

    def benchmark_login(self) -> None:
        fixtures = [self.create_user(f"login{i}")
                    for i in range(self.args.workers)]
        clients = [ApiClient(self.args.host, self.args.port, self.args.timeout)
                   for _ in fixtures]

        def operation(worker: int, _index: int) -> None:
            fixture = fixtures[worker]
            clients[worker].login(fixture.username, fixture.password)

        metric = self.run_parallel(
            "login-session-write", self.args.workers,
            self.args.operations, operation)
        self.results.append(metric.summary())
        for client in clients:
            client.close()

    def benchmark_conversation_list(self) -> None:
        fixtures = [self.create_pair(f"list{i}")
                    for i in range(self.args.workers)]

        def operation(worker: int, _index: int) -> None:
            status, data = fixtures[worker].sender.api.request(
                "GET", "/api/conversations")
            if status != 200 or not isinstance(data, list) or not data:
                raise AssertionError((status, data))

        metric = self.run_parallel(
            "conversation-list", self.args.workers,
            self.args.operations, operation)
        self.results.append(metric.summary())

    def _message_channels(self, pairs: list[PairFixture]):
        channels = []
        for pair in pairs:
            sender = WebSocketClient(pair.sender.api)
            receiver = WebSocketClient(pair.receiver.api)
            sender.connect()
            receiver.connect()
            channels.append((sender, receiver))
        return channels

    def benchmark_direct_delivery(self) -> None:
        pairs = [self.create_pair(f"direct{i}")
                 for i in range(self.args.workers)]
        channels = self._message_channels(pairs)
        accepted: list[float] = []
        accepted_lock = threading.Lock()

        def operation(worker: int, index: int) -> None:
            pair = pairs[worker]
            sender, receiver = channels[worker]
            message_id = f"direct-{worker}-{index}-{self.suffix}"
            started = time.perf_counter()
            sender.send_json({
                "v": 1, "type": "chat.send", "id": message_id,
                "conversationId": pair.conversation,
                "content": "phase15 durable direct message",
            })
            reply = sender.wait("chat.accepted", message_id)
            with accepted_lock:
                accepted.append((time.perf_counter() - started) * 1000.0)
            if reply.get("status") != "inserted":
                raise AssertionError(reply)
            receiver.wait("chat.message", client_message_id=message_id)

        metric = self.run_parallel(
            "direct-message-end-to-end", self.args.workers,
            self.args.operations, operation)
        metric.extra["databaseAcceptedLatencyMs"] = {
            "p50": round(percentile(accepted, 0.50), 3),
            "p95": round(percentile(accepted, 0.95), 3),
            "p99": round(percentile(accepted, 0.99), 3),
        }
        self.results.append(metric.summary())
        for sender, receiver in channels:
            sender.close()
            receiver.close()

    def benchmark_group_delivery(self) -> None:
        owner = self.create_user("group-owner")
        members = [self.create_user(f"group{i}")
                   for i in range(self.args.group_size)]
        status, conversation = owner.api.request(
            "POST", "/api/conversations",
            {"kind": "group", "title": "Phase15 Benchmark",
             "memberIds": ",".join(str(item.api.user_id) for item in members)})
        if status != 201 or not isinstance(conversation, dict):
            raise AssertionError(("group", status, conversation))
        conversation_id = int(conversation["id"])
        owner_ws = WebSocketClient(owner.api)
        owner_ws.connect()
        member_ws = [WebSocketClient(item.api) for item in members]
        for client in member_ws:
            client.connect()
        metric = Metric("group-message-all-recipients")
        accepted: list[float] = []
        metric.begin()
        try:
            for index in range(self.args.group_operations):
                message_id = f"group-{index}-{self.suffix}"
                started = time.perf_counter()
                owner_ws.send_json({
                    "v": 1, "type": "chat.send", "id": message_id,
                    "conversationId": conversation_id,
                    "content": "phase15 group fanout",
                })
                reply = owner_ws.wait("chat.accepted", message_id)
                accepted.append((time.perf_counter() - started) * 1000.0)
                if reply.get("status") != "inserted":
                    raise AssertionError(reply)
                for client in member_ws:
                    client.wait("chat.message", client_message_id=message_id)
                metric.observe(started, len(member_ws))
        except BaseException as error:
            metric.failure(error)
        finally:
            metric.finish()
            owner_ws.close()
            for client in member_ws:
                client.close()
        metric.extra["groupSize"] = len(member_ws)
        metric.extra["databaseAcceptedLatencyMs"] = {
            "p50": round(percentile(accepted, 0.50), 3),
            "p95": round(percentile(accepted, 0.95), 3),
            "p99": round(percentile(accepted, 0.99), 3),
        }
        self.results.append(metric.summary())

    def benchmark_offline_history(self) -> None:
        pair = self.create_pair("offline")
        sender = WebSocketClient(pair.sender.api)
        sender.connect()
        try:
            for index in range(self.args.history_messages):
                message_id = f"offline-{index}-{self.suffix}"
                sender.send_json({
                    "v": 1, "type": "chat.send", "id": message_id,
                    "conversationId": pair.conversation,
                    "content": "phase15 offline history payload",
                })
                reply = sender.wait("chat.accepted", message_id)
                if reply.get("status") != "inserted":
                    raise AssertionError(reply)
        finally:
            sender.close()

        def operation(_worker: int, _index: int) -> None:
            status, history = pair.receiver.api.request(
                "GET", f"/api/conversations/{pair.conversation}/messages"
                       f"?after=0&limit={self.args.history_messages}")
            if status != 200 or not isinstance(history, list) \
                    or len(history) < self.args.history_messages:
                raise AssertionError((status, len(history) if isinstance(history, list)
                                      else history))

        metric = self.run_parallel(
            "offline-history-page", 1, self.args.history_reads, operation,
            self.args.history_messages)
        metric.extra["messagesPerPage"] = self.args.history_messages
        self.results.append(metric.summary())

    def benchmark_connection_churn(self) -> None:
        fixtures = [self.create_user(f"conn{i}")
                    for i in range(self.args.workers)]

        def ws_operation(worker: int, _index: int) -> None:
            client = WebSocketClient(fixtures[worker].api)
            client.connect()
            client.close()

        ws_metric = self.run_parallel(
            "websocket-handshake", self.args.workers,
            self.args.connection_operations, ws_operation)
        self.results.append(ws_metric.summary())

        def sse_operation(worker: int, _index: int) -> None:
            client = SseClient(fixtures[worker].api)
            client.connect()
            client.close()

        sse_metric = self.run_parallel(
            "sse-ready-reconnect", self.args.workers,
            self.args.connection_operations, sse_operation)
        self.results.append(sse_metric.summary())

    def benchmark_websocket_residency(self) -> None:
        """逐级保留长连接，记录连接成功率以及服务进程的增量资源。"""
        steps = self.args.websocket_connection_steps
        if not steps:
            return

        # 服务允许一个账号最多四个设备连接。容量测试按“用户多设备”建连接，
        # 既符合真实场景，也避免把大量 Argon2 注册开销混入连接容量测量。
        fixture_count = math.ceil(
            steps[-1] / self.args.websocket_connections_per_user)
        fixtures = [self.create_user(f"resident{i}")
                    for i in range(fixture_count)]
        sampler = ResourceSampler(self.args.server_pid)
        baseline = sampler.snapshot()
        clients: list[WebSocketClient] = []
        points: list[dict] = []
        metric = Metric("websocket-resident-connections")
        metric.begin()
        previous_target = 0
        try:
            for target in steps:
                batch = [
                    fixtures[index // self.args.websocket_connections_per_user]
                    for index in range(previous_target, target)
                ]
                previous_target = target

                def connect(fixture: UserFixture) -> tuple[WebSocketClient, float]:
                    client = WebSocketClient(fixture.api)
                    started = time.perf_counter()
                    client.connect()
                    return client, (time.perf_counter() - started) * 1000.0

                with concurrent.futures.ThreadPoolExecutor(
                        max_workers=min(self.args.websocket_open_workers,
                                        max(1, len(batch)))) as pool:
                    futures = [pool.submit(connect, fixture) for fixture in batch]
                    for future in concurrent.futures.as_completed(futures):
                        try:
                            client, latency = future.result()
                            clients.append(client)
                            metric.latencies_ms.append(latency)
                            metric.units += 1
                        except BaseException as error:
                            metric.failure(error)

                # 所有连接都发一个 Ping；写失败会计入错误，而不是只看 fd 数量。
                for client in clients:
                    try:
                        client.ping()
                    except BaseException as error:
                        metric.failure(error)
                time.sleep(self.args.websocket_settle_seconds)
                snapshot = sampler.snapshot()
                point = {
                    "requestedConnections": target,
                    "establishedConnections": len(clients),
                    "connectionErrors": metric.error_count,
                    "process": snapshot,
                }
                if baseline.get("available") and snapshot.get("available"):
                    delta_rss = int(snapshot["rssKiB"]) - int(baseline["rssKiB"])
                    delta_fds = (int(snapshot["fileDescriptors"]) -
                                 int(baseline["fileDescriptors"]))
                    point.update({
                        "deltaRssKiB": delta_rss,
                        "estimatedBytesPerConnection": round(
                            max(0, delta_rss) * 1024 / max(1, len(clients)), 1),
                        "deltaFileDescriptors": delta_fds,
                        "fileDescriptorsPerConnection": round(
                            max(0, delta_fds) / max(1, len(clients)), 3),
                    })
                points.append(point)
        finally:
            metric.finish()
            for client in clients:
                client.close()
        metric.extra["baselineProcess"] = baseline
        metric.extra["connectionScale"] = points
        self.results.append(metric.summary())

    def benchmark_mixed(self) -> None:
        pairs = [self.create_pair(f"mixed{i}")
                 for i in range(self.args.workers)]
        channels = self._message_channels(pairs)

        def operation(worker: int, index: int) -> None:
            pair = pairs[worker]
            choice = (index + worker) % 10
            if choice < 5:
                status, data = pair.sender.api.request("GET", "/api/conversations")
                if status != 200 or not isinstance(data, list):
                    raise AssertionError((status, data))
            elif choice < 7:
                status, data = pair.sender.api.request(
                    "GET", f"/api/conversations/{pair.conversation}/messages"
                           "?after=0&limit=20")
                if status != 200 or not isinstance(data, list):
                    raise AssertionError((status, data))
            else:
                sender, receiver = channels[worker]
                message_id = f"mixed-{worker}-{index}-{self.suffix}"
                sender.send_json({
                    "v": 1, "type": "chat.send", "id": message_id,
                    "conversationId": pair.conversation,
                    "content": "phase15 mixed workload",
                })
                if sender.wait("chat.accepted", message_id).get("status") != "inserted":
                    raise AssertionError("mixed send rejected")
                receiver.wait("chat.message", client_message_id=message_id)

        metric = self.run_parallel(
            "mixed-50-list-20-history-30-send", self.args.workers,
            self.args.operations, operation)
        self.results.append(metric.summary())
        for sender, receiver in channels:
            sender.close()
            receiver.close()

    def _create_ai_fixture(self, label: str) -> AiFixture:
        actor = self.create_user(label)
        status, conversation = actor.api.request(
            "POST", "/api/conversations",
            {"kind": "direct", "peerId": str(self.args.ai_user_id)})
        if status != 201 or not isinstance(conversation, dict):
            raise AssertionError(("AI conversation", status, conversation))
        websocket = WebSocketClient(actor.api)
        events = SseClient(actor.api)
        try:
            websocket.connect()
            events.connect()
        except BaseException:
            events.close()
            websocket.close()
            raise
        return AiFixture(actor, int(conversation["id"]), websocket, events)

    def _create_ai_fixtures(self, prefix: str, count: int) -> list[AiFixture]:
        fixtures: list[AiFixture] = []
        try:
            for index in range(count):
                fixtures.append(self._create_ai_fixture(f"{prefix}{index}"))
            return fixtures
        except BaseException:
            for fixture in fixtures:
                fixture.events.close()
                fixture.websocket.close()
            raise

    def _ai_generation(self, fixture: AiFixture, index: int, label: str,
                       start_gate: Optional[threading.Event] = None,
                       on_ready: Optional[Callable[[], None]] = None
                       ) -> tuple[float, float, int]:
        ws = fixture.websocket
        sse = fixture.events
        stop_reader = threading.Event()
        try:
            generation = f"p15-{label}-{index}-{self.suffix}"

            def drain_websocket() -> None:
                try:
                    while not stop_reader.is_set():
                        ws.read_json()
                except (OSError, AssertionError, socket.timeout):
                    pass

            reader = threading.Thread(target=drain_websocket, daemon=True)
            reader.start()
            if on_ready:
                on_ready()
            if start_gate and not start_gate.wait(self.args.timeout):
                raise TimeoutError("AI mixed-load start gate timed out")
            started = time.perf_counter()
            ws.send_json({
                "v": 1, "type": "ai.generate", "id": generation,
                "to": self.args.ai_user_id,
                "conversationId": fixture.conversation,
                "content": self.args.ai_prompt,
            })
            first = 0.0
            count = 0
            for _ in range(self.args.ai_max_events):
                event, data = sse.read_event()
                if event == "ai.token":
                    count += 1
                    if not first:
                        first = time.perf_counter()
                elif event == "ai.completed":
                    if not isinstance(data, dict):
                        raise AssertionError((event, data))
                    if not data.get("success"):
                        raise AssertionError(data)
                    total = time.perf_counter() - started
                    return ((first or time.perf_counter()) - started,
                            total, count)
            raise AssertionError("AI completion event missing")
        finally:
            stop_reader.set()
            sse.close()
            ws.close()

    @staticmethod
    def _complete_ai_metric(metric: Metric, ttft: list[float],
                            tokens: list[int], concurrency: int) -> None:
        metric.extra["concurrency"] = concurrency
        metric.extra["timeToFirstTokenMs"] = {
            "p50": round(percentile(ttft, 0.50), 3),
            "p95": round(percentile(ttft, 0.95), 3),
            "p99": round(percentile(ttft, 0.99), 3),
        }
        metric.extra["tokensPerGenerationAverage"] = (
            round(statistics.fmean(tokens), 2) if tokens else 0.0)

    def benchmark_ai(self) -> None:
        if not self.args.ai_user_id:
            return

        ttft: list[float] = []
        tokens: list[int] = []
        fixtures = self._create_ai_fixtures("ai", self.args.ai_jobs)

        metric = Metric("ai-generation")
        metric.begin()
        with concurrent.futures.ThreadPoolExecutor(
                max_workers=self.args.ai_concurrency) as pool:
            futures = [pool.submit(self._ai_generation, fixture, index, "ai")
                       for index, fixture in enumerate(fixtures)]
            for future in concurrent.futures.as_completed(futures):
                try:
                    first, total, token_count = future.result()
                    # total 已是生成耗时；直接写入而不是用 future 完成后的本地时间。
                    metric.latencies_ms.append(total * 1000.0)
                    metric.units += token_count
                    ttft.append(first * 1000.0)
                    tokens.append(token_count)
                except BaseException as error:
                    metric.failure(error)
        metric.finish()
        self._complete_ai_metric(metric, ttft, tokens,
                                 self.args.ai_concurrency)
        self.results.append(metric.summary())

    def benchmark_ai_mixed(self) -> None:
        """让 AI 生成与普通 HTTP/WS 聊天同时发生，观察隔离效果。"""
        if not self.args.ai_user_id or not self.args.ai_mixed_jobs:
            return

        pairs = [self.create_pair(f"aimix{i}")
                 for i in range(self.args.workers)]
        channels = self._message_channels(pairs)
        try:
            ai_fixtures = self._create_ai_fixtures(
                "mixai", self.args.ai_mixed_jobs)
        except BaseException:
            for sender, receiver in channels:
                sender.close()
                receiver.close()
            raise
        ready = threading.Condition()
        ready_count = 0
        gate = threading.Event()
        ai_metric = Metric("mixed-with-ai-generation")
        chat_metric = Metric("mixed-with-ai-chat")
        ttft: list[float] = []
        tokens: list[int] = []

        def mark_ready() -> None:
            nonlocal ready_count
            with ready:
                ready_count += 1
                ready.notify_all()

        def chat_operation(worker: int, index: int) -> None:
            pair = pairs[worker]
            choice = (index + worker) % 10
            if choice < 5:
                status, data = pair.sender.api.request("GET", "/api/conversations")
                if status != 200 or not isinstance(data, list):
                    raise AssertionError((status, data))
            elif choice < 7:
                status, data = pair.sender.api.request(
                    "GET", f"/api/conversations/{pair.conversation}/messages"
                           "?after=0&limit=20")
                if status != 200 or not isinstance(data, list):
                    raise AssertionError((status, data))
            else:
                sender, receiver = channels[worker]
                message_id = f"aimix-{worker}-{index}-{self.suffix}"
                sender.send_json({
                    "v": 1, "type": "chat.send", "id": message_id,
                    "conversationId": pair.conversation,
                    "content": "phase26 ordinary chat during AI generation",
                })
                if sender.wait("chat.accepted", message_id).get("status") != "inserted":
                    raise AssertionError("AI mixed send rejected")
                receiver.wait("chat.message", client_message_id=message_id)

        ai_metric.begin()
        try:
            with concurrent.futures.ThreadPoolExecutor(
                    max_workers=self.args.ai_mixed_concurrency) as pool:
                futures = [pool.submit(
                    self._ai_generation, fixture, index, "mixai", gate, mark_ready)
                    for index, fixture in enumerate(ai_fixtures)]
                expected_ready = min(self.args.ai_mixed_jobs,
                                     self.args.ai_mixed_concurrency)
                deadline = time.monotonic() + self.args.timeout
                with ready:
                    while ready_count < expected_ready:
                        if all(future.done() for future in futures):
                            break
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            break
                        ready.wait(min(0.1, remaining))
                gate.set()
                chat_metric = self.run_parallel(
                    "mixed-with-ai-chat", self.args.workers,
                    self.args.ai_mixed_chat_operations, chat_operation)
                for future in concurrent.futures.as_completed(futures):
                    try:
                        first, total, token_count = future.result()
                        ai_metric.latencies_ms.append(total * 1000.0)
                        ai_metric.units += token_count
                        ttft.append(first * 1000.0)
                        tokens.append(token_count)
                    except BaseException as error:
                        ai_metric.failure(error)
        finally:
            gate.set()
            ai_metric.finish()
            for sender, receiver in channels:
                sender.close()
                receiver.close()
        self._complete_ai_metric(ai_metric, ttft, tokens,
                                 self.args.ai_mixed_concurrency)
        ai_metric.extra["readyWorkers"] = ready_count
        self.results.append(chat_metric.summary())
        self.results.append(ai_metric.summary())

    def run(self) -> dict:
        sampler = ResourceSampler(self.args.server_pid)
        sampler.start()
        started = time.time()
        metrics_before = self.application_metrics()
        metrics_after: dict[str, float] = {}
        try:
            self.benchmark_login()
            self.benchmark_conversation_list()
            self.benchmark_direct_delivery()
            self.benchmark_group_delivery()
            self.benchmark_offline_history()
            self.benchmark_connection_churn()
            self.benchmark_websocket_residency()
            self.benchmark_mixed()
            self.benchmark_ai()
            self.benchmark_ai_mixed()
        finally:
            metrics_after = self.application_metrics()
            resources = sampler.stop()
            for user in self.users:
                user.api.close()
        metric_deltas = {
            name: round(value - metrics_before.get(name, 0.0), 3)
            for name, value in metrics_after.items()
            if name.endswith("_total")
        }
        return {
            "schema": "phase15-business-benchmark-v1",
            "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "target": f"http://{self.args.host}:{self.args.port}",
            "serverPid": self.args.server_pid or None,
            "prefix": f"p15_*_{self.suffix}",
            "configuration": {
                "workers": self.args.workers,
                "operations": self.args.operations,
                "groupSize": self.args.group_size,
                "groupOperations": self.args.group_operations,
                "historyMessages": self.args.history_messages,
                "historyReads": self.args.history_reads,
                "connectionOperations": self.args.connection_operations,
                "websocketConnectionSteps": self.args.websocket_connection_steps,
                "websocketOpenWorkers": self.args.websocket_open_workers,
                "websocketConnectionsPerUser": (
                    self.args.websocket_connections_per_user),
                "websocketSettleSeconds": self.args.websocket_settle_seconds,
                "aiUserId": self.args.ai_user_id or None,
                "aiJobs": self.args.ai_jobs if self.args.ai_user_id else 0,
                "aiConcurrency": self.args.ai_concurrency if self.args.ai_user_id else 0,
                "aiMixedJobs": (
                    self.args.ai_mixed_jobs if self.args.ai_user_id else 0),
                "aiMixedConcurrency": (
                    self.args.ai_mixed_concurrency if self.args.ai_user_id else 0),
                "aiMixedChatOperations": (
                    self.args.ai_mixed_chat_operations
                    if self.args.ai_user_id and self.args.ai_mixed_jobs else 0),
                "aiMaxEvents": (
                    self.args.ai_max_events if self.args.ai_user_id else 0),
                "aiPromptSha256": (
                    hashlib.sha256(self.args.ai_prompt.encode()).hexdigest()
                    if self.args.ai_user_id else None),
            },
            "elapsedSeconds": round(time.time() - started, 3),
            "results": self.results,
            "resources": resources,
            "applicationMetrics": {
                "before": metrics_before,
                "after": metrics_after,
                "counterDeltas": metric_deltas,
            },
        }


def positive(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def connection_steps(value: str) -> list[int]:
    """解析严格递增的连接阶梯；空字符串表示保持旧基准行为。"""
    if not value.strip():
        return []
    try:
        parsed = [int(item.strip()) for item in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "connection steps must be comma-separated integers") from error
    if any(item <= 0 for item in parsed):
        raise argparse.ArgumentTypeError("connection steps must be positive")
    if parsed != sorted(set(parsed)):
        raise argparse.ArgumentTypeError(
            "connection steps must be unique and strictly increasing")
    return parsed


def main() -> int:
    parser = argparse.ArgumentParser(description="Phase 15 real business benchmark")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=positive, default=18090)
    parser.add_argument("--server-pid", type=int, default=0)
    parser.add_argument("--workers", type=positive, default=2)
    parser.add_argument("--operations", type=positive, default=20)
    parser.add_argument("--group-size", type=positive, default=4)
    parser.add_argument("--group-operations", type=positive, default=8)
    parser.add_argument("--history-messages", type=positive, default=20)
    parser.add_argument("--history-reads", type=positive, default=10)
    parser.add_argument("--connection-operations", type=positive, default=10)
    parser.add_argument("--websocket-connection-steps", type=connection_steps,
                        default=[])
    parser.add_argument("--websocket-open-workers", type=positive, default=16)
    parser.add_argument("--websocket-connections-per-user", type=positive,
                        default=4)
    parser.add_argument("--websocket-settle-seconds", type=float, default=1.0)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--ai-user-id", type=int, default=0)
    parser.add_argument("--ai-jobs", type=positive, default=1)
    parser.add_argument("--ai-concurrency", type=positive, default=1)
    parser.add_argument("--ai-mixed-jobs", type=int, default=0)
    parser.add_argument("--ai-mixed-concurrency", type=positive, default=1)
    parser.add_argument("--ai-mixed-chat-operations", type=positive, default=20)
    parser.add_argument("--ai-max-events", type=positive, default=2048)
    parser.add_argument("--ai-prompt", default="用三句话解释 Reactor 模式。")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.timeout <= 0 or args.websocket_settle_seconds < 0:
        parser.error("timeout must be positive and settle seconds non-negative")
    if args.ai_mixed_jobs < 0:
        parser.error("ai mixed jobs must be non-negative")
    if args.websocket_connections_per_user > 4:
        parser.error("websocket connections per user must not exceed 4")

    report = BusinessBenchmark(args).run()
    encoded = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
    return 0 if all(item["errors"] == 0 for item in report["results"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
