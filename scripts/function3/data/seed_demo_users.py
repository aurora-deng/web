#!/usr/bin/env python3

"""通过 Phase 11 公开业务 API 创建演示用户、好友关系和群聊。

脚本只使用 Python 标准库。它故意不直连 PostgreSQL：注册、资料修改、
好友申请、接受申请和建群都走与浏览器相同的 HTTP 业务接口，因此也能作为
一条轻量的端到端验收路径。脚本可重复执行；已存在的账号、好友和群聊会跳过。
"""

from __future__ import annotations

import argparse
import http.client
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEMO_USERS = (
    ("nebula_owner", "林川", "产品与团队负责人 · 负责需求、架构与协作节奏"),
    ("suran_product", "苏然", "产品经理 · 负责需求梳理与版本规划"),
    ("zhangming_backend", "张明", "后端工程师 · C++ 网络服务与业务接口"),
    ("chenyu_frontend", "陈雨", "前端工程师 · 交互界面与实时通信联调"),
    ("wanglei_qa", "王磊", "测试工程师 · 自动化、边界与可靠性验证"),
    ("linxia_ops", "林夏", "运维工程师 · Linux、部署与可观测性"),
    ("lina_design", "李娜", "UI/UX 设计师 · 视觉规范与使用体验"),
    ("zhouyan_data", "周然", "数据工程师 · 数据治理与模型数据管线"),
    ("zhaoyu_security", "赵宇", "安全工程师 · 身份、权限与安全审计"),
    ("xiaoyu_guest", "小宇", "普通协作成员 · 可参与私聊与群聊"),
)


def read_secret(path: Path) -> str:
    """从仓库外的权限受控文件读取密码，避免把凭据写进源码。"""
    value = path.read_text(encoding="utf-8").strip()
    if len(value) < 8:
        raise ValueError(f"password file must contain at least 8 characters: {path}")
    return value


@dataclass
class ApiClient:
    host: str
    port: int
    cookie: str = ""
    csrf: str = ""
    user: dict[str, Any] | None = None

    def request(self, method: str, path: str,
                body: dict[str, str] | None = None) -> tuple[int, Any]:
        headers = {"Accept": "application/json"}
        payload = b""
        if body is not None:
            payload = json.dumps(body, ensure_ascii=False,
                                 separators=(",", ":")).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if self.cookie:
            headers["Cookie"] = self.cookie
        if method not in ("GET", "HEAD"):
            headers["Origin"] = f"http://{self.host}:{self.port}"
            if self.csrf:
                headers["X-CSRF-Token"] = self.csrf

        connection = http.client.HTTPConnection(self.host, self.port, timeout=10)
        connection.request(method, path, payload, headers)
        response = connection.getresponse()
        raw = response.read()
        set_cookie = response.getheader("Set-Cookie")
        if set_cookie:
            self.cookie = set_cookie.split(";", 1)[0]
        connection.close()
        parsed = json.loads(raw.decode("utf-8")) if raw else {}
        return response.status, parsed

    def expect(self, method: str, path: str, expected: tuple[int, ...],
               body: dict[str, str] | None = None) -> Any:
        status, result = self.request(method, path, body)
        if status not in expected:
            raise RuntimeError(f"{method} {path}: HTTP {status}: {result}")
        return result

    def register_or_login(self, username: str, password: str,
                          display_name: str, biography: str) -> bool:
        status, result = self.request("POST", "/api/auth/register", {
            "username": username,
            "password": password,
            "displayName": display_name,
        })
        created = status == 201
        if status not in (201, 409):
            raise RuntimeError(f"register {username}: HTTP {status}: {result}")

        login = self.expect("POST", "/api/auth/login", (200,), {
            "username": username,
            "password": password,
        })
        self.user = login["user"]
        self.csrf = login["csrfToken"]
        profile = self.expect("PATCH", "/api/me", (200,), {
            "displayName": display_name,
            "biography": biography,
        })
        self.user = profile["user"]
        return created

    @property
    def user_id(self) -> int:
        if not self.user:
            raise RuntimeError("client has not logged in")
        return int(self.user["id"])

    def friend_ids(self) -> set[int]:
        return {int(item["id"]) for item in self.expect(
            "GET", "/api/friends", (200,)
        )}

    def logout(self) -> None:
        if self.cookie and self.csrf:
            self.expect("POST", "/api/auth/logout", (200,), {})


def befriend(sender: ApiClient, receiver: ApiClient) -> bool:
    """完成“申请 -> 接受”，从业务入口建立对称好友关系。"""
    if receiver.user_id in sender.friend_ids():
        return False
    request = sender.expect("POST", "/api/friend-requests", (201,), {
        "receiverId": str(receiver.user_id),
    })
    accepted = receiver.expect(
        "POST", f"/api/friend-requests/{request['id']}", (200,),
        {"accept": "true"},
    )
    if accepted.get("state") != "accepted":
        raise RuntimeError(f"friend request {request['id']} was not accepted")
    return True


def ensure_group(owner: ApiClient, members: list[ApiClient], title: str) -> bool:
    conversations = owner.expect("GET", "/api/conversations", (200,))
    if any(item.get("kind") == "group" and item.get("title") == title
           for item in conversations):
        return False
    member_ids = ",".join(str(member.user_id) for member in members
                          if member.user_id != owner.user_id)
    owner.expect("POST", "/api/conversations", (201,), {
        "kind": "group",
        "title": title,
        "memberIds": member_ids,
    })
    return True


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18090)
    parser.add_argument(
        "--password-file", type=Path,
        default=Path("/home/pikachu/phase11-data/secrets/phase11-demo.password"),
        help="仓库外的演示账号密码文件",
    )
    parser.add_argument("--group-title", default="星云全栈协作组")
    parser.add_argument("--anchor-username",
                        help="可选：把一个已有普通账号也加入演示好友网络")
    parser.add_argument("--anchor-password-file", type=Path)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    password = read_secret(args.password_file)
    clients: list[ApiClient] = []
    created_users = 0
    created_friendships = 0
    try:
        for username, display_name, biography in DEMO_USERS:
            client = ApiClient(args.host, args.port)
            if client.register_or_login(username, password, display_name, biography):
                created_users += 1
            clients.append(client)

        # 两两建立好友关系。friendships 表只存一行规范化的 user_low/user_high，
        # 因此前端任一方向查询都能看到对方，并不存在“单向好友”。
        for left_index, left in enumerate(clients):
            for right in clients[left_index + 1:]:
                created_friendships += int(befriend(left, right))

        if args.anchor_username:
            if not args.anchor_password_file:
                raise ValueError("--anchor-username requires --anchor-password-file")
            anchor = ApiClient(args.host, args.port)
            anchor_password = read_secret(args.anchor_password_file)
            login = anchor.expect("POST", "/api/auth/login", (200,), {
                "username": args.anchor_username,
                "password": anchor_password,
            })
            anchor.user = login["user"]
            anchor.csrf = login["csrfToken"]
            for demo in clients:
                created_friendships += int(befriend(anchor, demo))
            clients.append(anchor)

        group_created = ensure_group(clients[0], clients, args.group_title)
        owner_friends = clients[0].friend_ids()
        print(json.dumps({
            "createdUsers": created_users,
            "demoUsers": [{"id": item.user_id,
                           "username": item.user["username"],
                           "displayName": item.user["displayName"]}
                          for item in clients[:len(DEMO_USERS)]],
            "createdFriendships": created_friendships,
            "ownerFriendCount": len(owner_friends),
            "groupCreated": group_created,
            "groupTitle": args.group_title,
            "anchorIncluded": bool(args.anchor_username),
        }, ensure_ascii=False, indent=2))
    finally:
        for client in clients:
            try:
                client.logout()
            except Exception as error:  # 清理失败不覆盖主要执行结果。
                print(f"warning: logout failed for user {client.user_id}: {error}")


if __name__ == "__main__":
    main()
