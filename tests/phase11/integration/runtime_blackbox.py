#!/usr/bin/env python3

"""Phase 11 真实 HTTP + WebSocket + SSE + Outbox 黑盒测试。

该脚本只使用 Python 标准库，连接一台已经启动且启用了 Phase 11 的服务器。
它不绕过网络层，也不直接写数据库，因此能验证浏览器实际经过的完整协议路径。
传入 --ai-user-id 时还会验证 WebSocket -> PostgreSQL -> ModelProvider -> SSE。
默认会校验伪 Ollama 的固定文本；真实模型可传空的
--ai-expected-text，此时仍会验证非空 token、成功终态和双向持久化。
"""

import argparse
import base64
import hashlib
import http.client
import json
import os
import socket
import threading
import time
from dataclasses import dataclass
from typing import Optional


TIMEOUT = 5.0


def masked_frame(payload: bytes, opcode: int = 0x1) -> bytes:
    """浏览器发给服务器的 WebSocket 帧必须使用掩码。"""
    mask = b"\x12\x34\x56\x78"
    encoded = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    size = len(payload)
    if size < 126:
        length = bytes((0x80 | size,))
    elif size <= 0xFFFF:
        length = bytes((0x80 | 126,)) + size.to_bytes(2, "big")
    else:
        length = bytes((0x80 | 127,)) + size.to_bytes(8, "big")
    return bytes((0x80 | opcode,)) + length + mask + encoded


def receive_exact(sock: socket.socket, size: int, initial: bytearray) -> bytes:
    while len(initial) < size:
        part = sock.recv(4096)
        if not part:
            raise AssertionError("WebSocket closed while a frame was being read")
        initial.extend(part)
    result = bytes(initial[:size])
    del initial[:size]
    return result


class WebSocketReader:
    def __init__(self, sock: socket.socket, initial: bytes = b"") -> None:
        self.sock = sock
        self.buffer = bytearray(initial)

    def read_json(self) -> dict:
        while True:
            header = receive_exact(self.sock, 2, self.buffer)
            opcode = header[0] & 0x0F
            size = header[1] & 0x7F
            if size == 126:
                size = int.from_bytes(receive_exact(self.sock, 2, self.buffer), "big")
            elif size == 127:
                size = int.from_bytes(receive_exact(self.sock, 8, self.buffer), "big")
            if header[1] & 0x80:
                raise AssertionError("server WebSocket frames must not be masked")
            payload = receive_exact(self.sock, size, self.buffer)
            if opcode == 0x9:
                # 真实模型首次加载可能跨越心跳周期。测试客户端要像
                # 浏览器一样回 Pong，不能把合法 Ping 误判为业务帧。
                if size > 125:
                    raise AssertionError("WebSocket ping payload is too large")
                self.sock.sendall(masked_frame(payload, opcode=0xA))
                continue
            if opcode == 0xA:
                continue
            if opcode == 0x8:
                raise AssertionError("WebSocket closed while waiting for a JSON message")
            if opcode != 0x1:
                raise AssertionError(
                    f"expected WebSocket text frame, got opcode {opcode}"
                )
            return json.loads(payload)


class ChunkReader:
    """按 HTTP/1.1 chunked 规则取出一整个 SSE 事件块。"""

    def __init__(self, sock: socket.socket, initial: bytes = b"") -> None:
        self.sock = sock
        self.buffer = bytearray(initial)

    def _fill(self, size: int) -> None:
        while len(self.buffer) < size:
            part = self.sock.recv(4096)
            if not part:
                raise AssertionError("SSE stream closed while reading a chunk")
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


def receive_headers(sock: socket.socket) -> tuple[bytes, bytes]:
    data = bytearray()
    while b"\r\n\r\n" not in data:
        part = sock.recv(4096)
        if not part:
            raise AssertionError("connection closed before HTTP headers completed")
        data.extend(part)
    header, tail = bytes(data).split(b"\r\n\r\n", 1)
    return header + b"\r\n\r\n", tail


@dataclass
class BrowserClient:
    host: str
    port: int
    cookie: str = ""
    csrf: str = ""
    user_id: int = 0

    def request(self, method: str, path: str,
                body: Optional[dict] = None) -> tuple[int, dict]:
        headers = {"Accept": "application/json"}
        encoded = b""
        if body is not None:
            encoded = json.dumps(body, separators=(",", ":")).encode()
            headers["Content-Type"] = "application/json"
        if self.cookie:
            headers["Cookie"] = self.cookie
        if method not in ("GET", "HEAD") and self.csrf:
            headers["X-CSRF-Token"] = self.csrf
            headers["Origin"] = "http://localhost"
        connection = http.client.HTTPConnection(self.host, self.port, timeout=TIMEOUT)
        connection.request(method, path, encoded, headers)
        response = connection.getresponse()
        raw = response.read()
        set_cookie = response.getheader("Set-Cookie")
        if set_cookie:
            self.cookie = set_cookie.split(";", 1)[0]
        connection.close()
        parsed = json.loads(raw) if raw else {}
        return response.status, parsed

    def register_and_login(self, username: str, password: str) -> None:
        status, registered = self.request(
            "POST", "/api/auth/register",
            {"username": username, "password": password, "displayName": username},
        )
        if status != 201:
            raise AssertionError(("register", status, registered))
        self.user_id = int(registered["user"]["id"])
        status, logged_in = self.request(
            "POST", "/api/auth/login", {"username": username, "password": password}
        )
        if status != 200:
            raise AssertionError(("login", status, logged_in))
        self.csrf = logged_in["csrfToken"]

    def open_websocket(self) -> tuple[socket.socket, WebSocketReader]:
        key = base64.b64encode(os.urandom(16)).decode()
        expected = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        )
        sock = socket.create_connection((self.host, self.port), timeout=TIMEOUT)
        sock.settimeout(TIMEOUT)
        sock.sendall(
            (
                f"GET /ws?uid={self.user_id} HTTP/1.1\r\n"
                f"Host: {self.host}:{self.port}\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {key}\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Origin: http://localhost\r\n"
                f"Cookie: {self.cookie}\r\n\r\n"
            ).encode()
        )
        header, tail = receive_headers(sock)
        if not header.startswith(b"HTTP/1.1 101 Switching Protocols"):
            raise AssertionError(("websocket handshake", header))
        if expected not in header:
            raise AssertionError("invalid Sec-WebSocket-Accept")
        return sock, WebSocketReader(sock, tail)

    def open_sse(self) -> tuple[socket.socket, ChunkReader]:
        sock = socket.create_connection((self.host, self.port), timeout=TIMEOUT)
        sock.settimeout(TIMEOUT)
        sock.sendall(
            (
                f"GET /events?uid={self.user_id} HTTP/1.1\r\n"
                f"Host: {self.host}:{self.port}\r\n"
                "Accept: text/event-stream\r\n"
                "Connection: keep-alive\r\n"
                "Origin: http://localhost\r\n"
                f"Cookie: {self.cookie}\r\n\r\n"
            ).encode()
        )
        header, tail = receive_headers(sock)
        if not header.startswith(b"HTTP/1.1 200 OK"):
            raise AssertionError(("sse handshake", header))
        reader = ChunkReader(sock, tail)
        ready = reader.read()
        if "event: ready\n" not in ready:
            raise AssertionError(("sse ready", ready))
        return sock, reader


def wait_for_ws(reader: WebSocketReader, event_type: str, attempts: int = 6) -> dict:
    observed = []
    for _ in range(attempts):
        message = reader.read_json()
        observed.append(message.get("type"))
        if message.get("type") == event_type:
            return message
    raise AssertionError((f"missing WebSocket event {event_type}", observed))


def wait_for_sse(reader: ChunkReader, event_name: str, attempts: int = 6) -> str:
    observed = []
    for _ in range(attempts):
        event = reader.read()
        observed.append(event.splitlines()[1:2])
        if f"event: {event_name}\n" in event:
            return event
    raise AssertionError((f"missing SSE event {event_name}", observed))


def sse_json(event: str) -> dict:
    data = [line[6:] for line in event.splitlines() if line.startswith("data: ")]
    if not data:
        raise AssertionError(("SSE event has no data", event))
    return json.loads("\n".join(data))


def run(host: str, port: int, ai_user_id: int = 0,
        ai_expected_text: str = "hello from fake ollama",
        ai_max_events: int = 1024,
        ai_expect_failure: bool = False) -> None:
    suffix = f"{time.time_ns() % 1_000_000_000:09d}"
    alice = BrowserClient(host, port)
    bob = BrowserClient(host, port)
    alice.register_and_login("phase11_alice_" + suffix, "correct-password")
    bob.register_and_login("phase11_bob_" + suffix, "correct-password")

    status, friend_request = alice.request(
        "POST", "/api/friend-requests", {"receiverId": str(bob.user_id)}
    )
    if status != 201:
        raise AssertionError(("friend request", status, friend_request))
    status, accepted = bob.request(
        "POST", f"/api/friend-requests/{friend_request['id']}", {"accept": "true"}
    )
    if status != 200 or accepted.get("state") != "accepted":
        raise AssertionError(("friend accept", status, accepted))

    status, conversation = alice.request(
        "POST", "/api/conversations", {"kind": "direct", "peerId": str(bob.user_id)}
    )
    if status != 201:
        raise AssertionError(("conversation", status, conversation))
    conversation_id = int(conversation["id"])
    status, participants = alice.request(
        "GET", f"/api/conversations/{conversation_id}/members"
    )
    participant_ids = {int(item["userId"]) for item in participants}
    if status != 200 or participant_ids != {alice.user_id, bob.user_id}:
        raise AssertionError(("conversation participants", status, participants))
    if any("displayName" not in item or "aiAccount" not in item
           for item in participants):
        raise AssertionError(("participant profiles missing", participants))

    alice_socket, alice_ws = alice.open_websocket()
    bob_socket, bob_ws = bob.open_websocket()
    bob_sse_socket, bob_sse = bob.open_sse()
    try:
        client_message_id = "runtime-" + suffix
        outbound = {
            "v": 1,
            "type": "chat.send",
            "id": client_message_id,
            "conversationId": conversation_id,
            "content": "hello durable realtime",
        }
        encoded = json.dumps(outbound, separators=(",", ":")).encode()
        alice_socket.sendall(masked_frame(encoded))

        accepted_ws = wait_for_ws(alice_ws, "chat.accepted")
        alice_event = wait_for_ws(alice_ws, "chat.message")
        bob_event = wait_for_ws(bob_ws, "chat.message")
        sse_event = wait_for_sse(bob_sse, "message.notification")
        if accepted_ws.get("status") != "inserted":
            raise AssertionError(("first send was not inserted", accepted_ws))
        if alice_event.get("body") != "hello durable realtime" or bob_event != alice_event:
            raise AssertionError(("broadcast mismatch", alice_event, bob_event))
        if "hello durable realtime" not in sse_event:
            raise AssertionError(("SSE payload mismatch", sse_event))

        bob_socket.sendall(masked_frame(json.dumps({
            "v": 1,
            "type": "chat.ack",
            "id": "ack-" + suffix,
            "conversationId": conversation_id,
            "sequence": int(bob_event["sequence"]),
        }, separators=(",", ":")).encode()))
        if wait_for_ws(bob_ws, "chat.acknowledged").get("status") != "accepted":
            raise AssertionError("chat ACK was rejected")

        # 相同 client message id 重发时必须命中幂等记录，不能生成第二条消息。
        alice_socket.sendall(masked_frame(encoded))
        duplicate = wait_for_ws(alice_ws, "chat.accepted")
        if duplicate.get("status") != "duplicate":
            raise AssertionError(("duplicate was inserted", duplicate))
        status, history = bob.request(
            "GET", f"/api/conversations/{conversation_id}/messages?after=0&limit=100"
        )
        matches = [item for item in history if item.get("clientMessageId") == client_message_id]
        if status != 200 or len(matches) != 1:
            raise AssertionError(("idempotent history", status, matches))
    finally:
        alice_socket.close()
        bob_socket.close()
        bob_sse_socket.close()

    ai_result = None
    if ai_user_id:
        status, ai_conversation = alice.request(
            "POST", "/api/conversations",
            {"kind": "direct", "peerId": str(ai_user_id)},
        )
        if status != 201:
            raise AssertionError(("AI conversation", status, ai_conversation))
        ai_conversation_id = int(ai_conversation["id"])
        status, ai_participants = alice.request(
            "GET", f"/api/conversations/{ai_conversation_id}/members"
        )
        selected_ai = next(
            (item for item in ai_participants
             if int(item["userId"]) == ai_user_id),
            None,
        )
        if status != 200 or not selected_ai or not selected_ai.get("aiAccount"):
            raise AssertionError(("AI participant profile", status, ai_participants))
        ai_socket, ai_ws = alice.open_websocket()
        ai_sse_socket, ai_sse = alice.open_sse()
        try:
            generation_id = "ai-runtime-" + suffix
            ai_socket.sendall(masked_frame(json.dumps({
                "v": 1,
                "type": "ai.generate",
                "id": generation_id,
                "to": ai_user_id,
                "conversationId": ai_conversation_id,
                "content": "explain the reactor briefly",
            }, separators=(",", ":")).encode()))
            if wait_for_ws(ai_ws, "ai.accepted").get("status") != "accepted":
                raise AssertionError("AI request was not accepted")

            # AI 首次加载可能超过一个 WebSocket 心跳周期。真实浏览器会在网络层
            # 自动读取 Ping 并回复 Pong；测试脚本也必须同时消费 WebSocket，不能在
            # 阻塞读取 SSE 时把另一条长连接晾在一边。
            ws_messages = []
            ws_errors = []

            def collect_ai_messages() -> None:
                try:
                    ws_messages.extend(
                        wait_for_ws(ai_ws, "chat.message", 12) for _ in range(2)
                    )
                except BaseException as failure:  # 传回主测试线程统一报告。
                    ws_errors.append(failure)

            ws_thread = threading.Thread(
                target=collect_ai_messages,
                name="phase11-ai-websocket-reader",
                daemon=True,
            )
            ws_thread.start()

            # 真实模型的 token 数不固定，不能像伪 Ollama 测试那样假定
            # “恰好两个 token”。持续读取，直到 ai.completed 为止。
            token_parts = []
            completed = None
            observed_events = []
            for _ in range(ai_max_events):
                event = ai_sse.read()
                event_name = next(
                    (line[7:] for line in event.splitlines()
                     if line.startswith("event: ")),
                    "",
                )
                observed_events.append(event_name)
                if event_name == "ai.token":
                    token_parts.append(sse_json(event).get("text", ""))
                elif event_name == "ai.completed":
                    completed = sse_json(event)
                    break
            if completed is None:
                raise AssertionError(("AI completion event missing", observed_events))
            generated = "".join(token_parts)
            # 先报告 Provider 的终态；失败时通常没有 token，若先检查空文本会
            # 把真正的模型加载/Adapter 错误遮成一个没有诊断价值的断言。
            if ai_expect_failure:
                if completed.get("success") is not False or completed.get("cancelled"):
                    raise AssertionError(("AI failure was not isolated", completed))
                if not completed.get("error"):
                    raise AssertionError(("AI failure has no diagnostic", completed))
                prompt_message = wait_for_ws(ai_ws, "chat.message", 12)
                if prompt_message.get("clientMessageId") != generation_id:
                    raise AssertionError(("AI failure prompt missing", prompt_message))
                status, history = alice.request(
                    "GET",
                    f"/api/conversations/{ai_conversation_id}/messages?after=0&limit=100",
                )
                if status != 200 or len(history) != 1 or \
                        history[0].get("clientMessageId") != generation_id:
                    raise AssertionError(("AI failure history", status, history))
                ai_result = {
                    "aiUser": ai_user_id,
                    "conversation": ai_conversation_id,
                    "generation": generation_id,
                    "expectedFailure": True,
                    "error": completed["error"],
                }
            else:
                if completed.get("success") is not True or completed.get("cancelled"):
                    raise AssertionError(("AI completion failed", completed, generated))
                if not generated:
                    raise AssertionError(("AI model returned no token text", completed))
                if ai_expected_text and generated != ai_expected_text:
                    raise AssertionError(("AI token stream mismatch", generated))

                ws_thread.join(timeout=10.0)
                if ws_thread.is_alive():
                    raise AssertionError("AI WebSocket messages did not arrive")
                if ws_errors:
                    raise ws_errors[0]
                messages = ws_messages
                by_client_id = {item.get("clientMessageId"): item for item in messages}
                if by_client_id.get(generation_id, {}).get("body") != \
                        "explain the reactor briefly":
                    raise AssertionError(("AI prompt was not persisted", messages))
                if by_client_id.get("ai:" + generation_id, {}).get("body") != generated:
                    raise AssertionError(("AI answer was not persisted", messages))

                status, history = alice.request(
                    "GET",
                    f"/api/conversations/{ai_conversation_id}/messages?after=0&limit=100",
                )
                if status != 200 or len(history) != 2:
                    raise AssertionError(("AI history", status, history))
                ai_result = {
                    "aiUser": ai_user_id,
                    "conversation": ai_conversation_id,
                    "generation": generation_id,
                    "tokens": len(token_parts),
                    "bytes": len(generated.encode("utf-8")),
                }
        finally:
            ai_socket.close()
            ai_sse_socket.close()

    print(
        "PHASE11_RUNTIME_BLACKBOX_OK",
        json.dumps({
            "alice": alice.user_id,
            "bob": bob.user_id,
            "conversation": conversation_id,
            "sequence": bob_event["sequence"],
            "ai": ai_result,
        }, separators=(",", ":")),
    )


def main() -> int:
    global TIMEOUT
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--ai-user-id", type=int, default=0)
    parser.add_argument(
        "--ai-expected-text", default="hello from fake ollama",
        help="exact AI text; pass an empty value to accept any non-empty model output",
    )
    parser.add_argument("--ai-max-events", type=int, default=1024)
    parser.add_argument(
        "--ai-expect-failure", action="store_true",
        help="require a controlled non-cancelled AI failure and verify normal chat survives",
    )
    parser.add_argument("--timeout", type=float, default=TIMEOUT)
    args = parser.parse_args()
    if args.timeout <= 0 or args.ai_max_events <= 0:
        parser.error("timeout and ai-max-events must be positive")
    TIMEOUT = args.timeout
    run(args.host, args.port, args.ai_user_id,
        args.ai_expected_text, args.ai_max_events, args.ai_expect_failure)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
