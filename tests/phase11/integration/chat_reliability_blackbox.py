#!/usr/bin/env python3
"""真实 HTTP/WS/数据库回归；只连接显式指定的隔离测试服务器。

忽略一次 accepted 模拟客户端未收到 ACK，随后查询并重放原幂等命令。
浏览器计时/重连交互另外由 frontend/chat_browser_smoke.cjs 覆盖。
"""
import argparse
import json
import time
from runtime_blackbox import BrowserClient, masked_frame


def run(host, port):
    suffix = str(time.time_ns())
    users = [BrowserClient(host, port) for _ in range(4)]
    for index, client in enumerate(users):
        client.register_and_login(f"reliable{index}_{suffix}", "test-only-password")
    owner, admin, member, outsider = users
    status, group = owner.request("POST", "/api/conversations", {
        "kind": "group", "title": "Phase27 isolated test",
        "memberIds": f"{admin.user_id},{member.user_id}"})
    assert status == 201, (status, group)
    conv = int(group["id"])
    status, response = owner.request("POST", f"/api/conversations/{conv}/members/{admin.user_id}/role",
                                     {"role": "administrator"})
    assert status == 200, (status, response)
    sockets = []

    def connect(client):
        sock, reader = client.open_websocket()
        sockets.append(sock)
        return sock, reader

    def command(sock, reader, value, kind):
        sock.sendall(masked_frame(json.dumps(value, separators=(",", ":")).encode()))
        # 业务事件、收据和 accepted 可能交错；通过 replyTo 定位这次命令。
        for _ in range(1500):
            event = reader.read_json()
            if event.get("type") == kind and event.get("replyTo") == value["id"]:
                return event
        raise AssertionError(("command reply missing", value, kind))

    member_sock, member_reader = connect(member)
    try:
        for seq in range(1, 232):
            accepted = command(member_sock, member_reader, {"v": 1, "type": "chat.send",
                "id": f"page-{seq}", "conversationId": conv, "content": f"message-{seq}"}, "chat.accepted")
            assert int(accepted["sequence"]) == seq and accepted["status"] == "inserted", accepted
        status, latest = owner.request("GET", f"/api/conversations/{conv}/messages?before=0&limit=100")
        assert status == 200 and [int(m["sequence"]) for m in latest] == list(range(132, 232)), (status, latest[:1])
        status, previous = owner.request("GET", f"/api/conversations/{conv}/messages?before=132&limit=100")
        assert status == 200 and [int(m["sequence"]) for m in previous] == list(range(32, 132))
        status, earliest = owner.request("GET", f"/api/conversations/{conv}/messages?before=32&limit=100")
        assert status == 200 and [int(m["sequence"]) for m in earliest] == list(range(1, 32))
        assert owner.request("GET", f"/api/conversations/{conv}/messages?before=1&limit=100") == (200, [])
        assert owner.request("GET", f"/api/conversations/{conv}/messages?before=1&after=1")[0] == 400
        assert outsider.request("GET", f"/api/conversations/{conv}/messages?before=0")[0] == 403
        print("PASS real latest/backward pagination, exclusive cursor, and membership checks")

        key = {"v": 1, "type": "chat.send", "id": "lost-ack", "conversationId": conv, "content": "one durable copy"}
        ignored = command(member_sock, member_reader, key, "chat.accepted")
        status, found = member.request("GET", f"/api/conversations/{conv}/messages/by-client-id?clientMessageId=lost-ack")
        assert status == 200 and str(found["id"]) == ignored["id"]
        assert owner.request("GET", f"/api/conversations/{conv}/messages/by-client-id?clientMessageId=lost-ack") == (200, None)
        assert member.request("GET", f"/api/conversations/{conv}/messages/by-client-id?clientMessageId=absent") == (200, None)
        assert outsider.request("GET", f"/api/conversations/{conv}/messages/by-client-id?clientMessageId=lost-ack")[0] == 403
        repeated = command(member_sock, member_reader, key, "chat.accepted")
        assert repeated["status"] == "duplicate" and repeated["id"] == ignored["id"]
        conflict = command(member_sock, member_reader, {**key, "content": "different content"}, "chat.error")
        assert conflict.get("status"), conflict
        print("PASS real ACK reconciliation, sender isolation, duplicate replay and content conflict")

        admin_sock, admin_reader = connect(admin)
        recalled = command(admin_sock, admin_reader, {"v": 1, "type": "chat.recall", "id": "admin-recall",
            "conversationId": conv, "replyTo": str(latest[-1]["id"])}, "chat.recall.accepted")
        assert recalled["status"] == "recalled"
        owner_sock, owner_reader = connect(owner)
        owner_message = command(owner_sock, owner_reader, {"v": 1, "type": "chat.send", "id": "owner-message",
            "conversationId": conv, "content": "owner message"}, "chat.accepted")
        denied = command(admin_sock, admin_reader, {"v": 1, "type": "chat.recall", "id": "admin-denied",
            "conversationId": conv, "replyTo": owner_message["id"]}, "chat.error")
        assert denied.get("status"), denied
        member_sock.close()
        for seq in range(234, 363):
            result = command(owner_sock, owner_reader, {"v": 1, "type": "chat.send", "id": f"offline-{seq}",
                "conversationId": conv, "content": f"offline-{seq}"}, "chat.accepted")
            assert int(result["sequence"]) == seq
        connect(member)
        recovered = []
        after = 233
        while after < 362:
            status, page = member.request("GET", f"/api/conversations/{conv}/messages?after={after}&limit=100")
            assert status == 200 and page, (status, page)
            recovered.extend(page)
            after = int(page[-1]["sequence"])
        assert [int(m["sequence"]) for m in recovered] == list(range(234, 363))
        status, refreshed = member.request("GET", f"/api/conversations/{conv}/messages?after=230&limit=5")
        assert status == 200 and refreshed[0]["state"] == "recalled" and not refreshed[0]["body"]
        print("PASS real administrator authorization, offline 129-message catch-up, and recalled history")
    finally:
        for sock in sockets:
            sock.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    run(args.host, args.port)
