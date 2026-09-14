#!/usr/bin/env python3

import argparse
import base64
import hashlib
import json
import os
import signal
import socket
import subprocess
import tempfile
import time
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


def masked_frame(opcode: int, payload: bytes, *, fin: bool = True) -> bytes:
    mask = b"\x12\x34\x56\x78"
    encoded = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    first = (0x80 if fin else 0) | opcode
    length = len(payload)
    if length < 126:
        header = bytes((first, 0x80 | length))
    elif length <= 0xFFFF:
        header = bytes((first, 0x80 | 126)) + length.to_bytes(2, "big")
    else:
        header = bytes((first, 0x80 | 127)) + length.to_bytes(8, "big")
    return header + mask + encoded


def receive_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise AssertionError("connection closed during frame")
        data.extend(chunk)
    return bytes(data)


def receive_frame(sock: socket.socket) -> tuple[bool, int, bytes]:
    header = receive_exact(sock, 2)
    length = header[1] & 0x7F
    if length == 126:
        length = int.from_bytes(receive_exact(sock, 2), "big")
    elif length == 127:
        length = int.from_bytes(receive_exact(sock, 8), "big")
    return bool(header[0] & 0x80), header[0] & 0x0F, receive_exact(sock, length)


def open_websocket(uid: int) -> socket.socket:
    key = base64.b64encode(f"ws-test-{uid:08d}".encode()).decode()
    expected_accept = base64.b64encode(
        hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()
        ).digest()
    )
    sock = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
    sock.settimeout(TIMEOUT)
    request = (
        f"GET /ws?uid={uid} HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    ).encode()
    sock.sendall(request)
    response = bytearray()
    while b"\r\n\r\n" not in response:
        chunk = sock.recv(1024)
        if not chunk:
            sock.close()
            raise AssertionError("connection closed during WebSocket handshake")
        response.extend(chunk)
    if b"HTTP/1.1 101 Switching Protocols" not in response:
        sock.close()
        raise AssertionError(response)
    if expected_accept not in response:
        sock.close()
        raise AssertionError("invalid Sec-WebSocket-Accept")
    return sock


def close_code(payload: bytes) -> int:
    if len(payload) < 2:
        raise AssertionError("close frame did not include a status code")
    return int.from_bytes(payload[:2], "big")


def run_check() -> None:
    # 基础升级、回显与 Close 握手。
    with open_websocket(9001) as sock:
        sock.sendall(masked_frame(0x1, b"hello"))
        fin, opcode, payload = receive_frame(sock)
        if not fin or opcode != 0x1 or b"hello" not in payload:
            raise AssertionError((fin, opcode, payload))

        sock.sendall(masked_frame(0x8, b"\x03\xe8"))
        _, close_opcode, close_payload = receive_frame(sock)
        if close_opcode != 0x8 or close_code(close_payload) != 1000:
            raise AssertionError("server did not drain close frame")

    # 空首分片也会开启消息；Ping 可穿插其中，Continuation 完成后再派发。
    with open_websocket(9002) as sock:
        sock.sendall(masked_frame(0x1, b"", fin=False))
        sock.sendall(masked_frame(0x9, b"mid"))
        fin, opcode, payload = receive_frame(sock)
        if not fin or opcode != 0xA or payload != b"mid":
            raise AssertionError("control frame was not handled during fragmentation")
        sock.sendall(masked_frame(0x0, b"fragmented", fin=True))
        fin, opcode, payload = receive_frame(sock)
        if not fin or opcode != 0x1 or b"fragmented" not in payload:
            raise AssertionError("empty first fragment was not assembled")

    # 分片消息未结束时插入新的数据首帧，服务端应回 1002。
    with open_websocket(9003) as sock:
        sock.sendall(masked_frame(0x1, b"part", fin=False))
        sock.sendall(masked_frame(0x2, b"new", fin=True))
        _, opcode, payload = receive_frame(sock)
        if opcode != 0x8 or close_code(payload) != 1002:
            raise AssertionError("invalid fragment sequence did not close with 1002")

    # 每帧都小于 1 MiB，但重组总量超过 1 MiB，不能靠拆帧绕过消息上限。
    with open_websocket(9004) as sock:
        sock.sendall(masked_frame(0x2, b"a" * (600 * 1024), fin=False))
        sock.sendall(masked_frame(0x0, b"b" * (600 * 1024), fin=True))
        _, opcode, payload = receive_frame(sock)
        if opcode != 0x8 or close_code(payload) != 1009:
            raise AssertionError("oversized fragmented message did not close with 1009")

    # UTF-8 码点可以跨帧；必须先重组整条 Text 消息，再做字符合法性判断。
    with open_websocket(9005) as sock:
        sock.sendall(masked_frame(0x1, b"\xe4", fin=False))
        sock.sendall(masked_frame(0x0, b"\xb8\xad", fin=True))
        fin, opcode, payload = receive_frame(sock)
        if not fin or opcode != 0x1 or b"\xe4\xb8\xad" not in payload:
            raise AssertionError("valid split UTF-8 code point was rejected")

    # 过长编码不是合法 UTF-8；完整 Text 消息应以 1007 关闭。
    with open_websocket(9006) as sock:
        sock.sendall(masked_frame(0x1, b"\xc0\xaf"))
        _, opcode, payload = receive_frame(sock)
        if opcode != 0x8 or close_code(payload) != 1007:
            raise AssertionError("invalid text UTF-8 did not close with 1007")

    # Close payload 只有 1 字节，连状态码都不完整，属于协议错误 1002。
    with open_websocket(9007) as sock:
        sock.sendall(masked_frame(0x8, b"\x03"))
        _, opcode, payload = receive_frame(sock)
        if opcode != 0x8 or close_code(payload) != 1002:
            raise AssertionError("one-byte close payload did not close with 1002")

    # 1005 只能表示“本地没有收到状态码”，不能真正写入网络帧。
    with open_websocket(9008) as sock:
        sock.sendall(masked_frame(0x8, (1005).to_bytes(2, "big")))
        _, opcode, payload = receive_frame(sock)
        if opcode != 0x8 or close_code(payload) != 1002:
            raise AssertionError("reserved close code did not close with 1002")

    # Close reason 也是文本；非法 UTF-8 应与结构错误区分，使用 1007。
    with open_websocket(9009) as sock:
        sock.sendall(masked_frame(0x8, (1000).to_bytes(2, "big") + b"\xc0\xaf"))
        _, opcode, payload = receive_frame(sock)
        if opcode != 0x8 or close_code(payload) != 1007:
            raise AssertionError("invalid close reason did not close with 1007")

    # 对端没有携带状态码时，服务端也应回应空 Close，而不是擅自补成 1000。
    with open_websocket(9010) as sock:
        sock.sendall(masked_frame(0x8, b""))
        _, opcode, payload = receive_frame(sock)
        if opcode != 0x8 or payload:
            raise AssertionError("empty close payload was not echoed as empty")

    # 跨 Reactor 私聊返回的是“准入回执”：accepted 表示目标 Reactor 已接单。
    with open_websocket(9011) as sender, open_websocket(9012) as receiver:
        receiver.sendall(masked_frame(0x9, b"ready"))
        _, opcode, payload = receive_frame(receiver)
        if opcode != 0xA or payload != b"ready":
            raise AssertionError("target WebSocket session was not ready")

        sender.sendall(masked_frame(0x1, b"@9012:mailbox-test"))
        _, opcode, payload = receive_frame(receiver)
        if opcode != 0x1 or payload != b"mailbox-test":
            raise AssertionError("accepted direct message did not reach target")
        _, opcode, payload = receive_frame(sender)
        if opcode != 0x1 or payload != b"accepted":
            raise AssertionError("sender did not receive accepted admission result")

        sender.sendall(masked_frame(0x1, b"@999999:offline"))
        _, opcode, payload = receive_frame(sender)
        if opcode != 0x1 or payload != b"target unavailable":
            raise AssertionError("offline target did not return an explicit result")

    # 应用层可靠投递：客户端 id 去重，服务端 id 关联 ACK，且 ACK 后重发不重复投递。
    with open_websocket(9013) as sender, open_websocket(9014) as receiver:
        request = {
            "v": 1,
            "type": "chat",
            "id": "client-1",
            "to": 9014,
            "content": "reliable \"message\"\n你好",
        }
        sender.sendall(masked_frame(
            0x1,
            json.dumps(request, ensure_ascii=False, separators=(",", ":")).encode(),
        ))

        _, opcode, payload = receive_frame(receiver)
        if opcode != 0x1:
            raise AssertionError("reliable chat was not a text frame")
        forwarded = json.loads(payload)
        server_id = forwarded.get("id", "")
        if not server_id.startswith("ws-9013-"):
            raise AssertionError(("missing server message id", forwarded))
        if forwarded != {
            "v": 1,
            "type": "chat",
            "id": server_id,
            "from": 9013,
            "to": 9014,
            "ack": True,
            "content": request["content"],
        }:
            raise AssertionError(("invalid forwarded envelope", forwarded))

        _, opcode, payload = receive_frame(sender)
        admission = json.loads(payload)
        if opcode != 0x1 or admission.get("status") != "accepted":
            raise AssertionError(("missing admission receipt", admission))
        if admission.get("id") != server_id or admission.get("replyTo") != "client-1":
            raise AssertionError(("receipt correlation mismatch", admission))

        ack = {"v": 1, "type": "ack", "replyTo": server_id}
        receiver.sendall(masked_frame(
            0x1,
            json.dumps(ack, separators=(",", ":")).encode(),
        ))

        _, opcode, payload = receive_frame(sender)
        acknowledged = json.loads(payload)
        if opcode != 0x1 or acknowledged.get("status") != "acknowledged":
            raise AssertionError(("sender did not receive application ACK", acknowledged))
        if acknowledged.get("id") != server_id:
            raise AssertionError(("ACK referenced a different server id", acknowledged))

        _, opcode, payload = receive_frame(receiver)
        ack_result = json.loads(payload)
        if opcode != 0x1 or ack_result.get("status") != "accepted":
            raise AssertionError(("recipient did not receive ACK result", ack_result))

        # 网络抖动后客户端可能重发同一请求；Tracker 返回已确认状态，不再投递给收件人。
        sender.sendall(masked_frame(
            0x1,
            json.dumps(request, ensure_ascii=False, separators=(",", ":")).encode(),
        ))
        _, opcode, payload = receive_frame(sender)
        duplicate = json.loads(payload)
        if opcode != 0x1 or duplicate.get("status") != "acknowledged":
            raise AssertionError(("duplicate request lost terminal state", duplicate))

        receiver.settimeout(0.25)
        try:
            receive_frame(receiver)
            raise AssertionError("duplicate request was delivered twice")
        except socket.timeout:
            pass
        finally:
            receiver.settimeout(TIMEOUT)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, type=Path)
    server = parser.parse_args().server.resolve()
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen(
            [str(server)],
            cwd=server.parent,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            wait_ready(process)
            run_check()
            print("all WebSocket black-box checks passed")
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
