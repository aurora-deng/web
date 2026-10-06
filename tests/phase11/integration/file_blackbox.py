#!/usr/bin/env python3

"""Phase 17 file API black box using only Python's standard library.

The test talks to an already-running server through HTTP.  It deliberately keeps
Bob offline while Alice sends the attachment, proving that delivery is persisted
business data rather than a best-effort WebSocket event.
"""

import argparse
import http.client
import json
import time
from dataclasses import dataclass
from typing import Any, Optional


@dataclass
class Response:
    status: int
    headers: dict[str, str]
    body: bytes

    def json(self) -> Any:
        return json.loads(self.body) if self.body else {}


@dataclass
class Client:
    host: str
    port: int
    cookie: str = ""
    csrf: str = ""
    user_id: int = 0

    def request(
        self,
        method: str,
        path: str,
        body: bytes = b"",
        headers: Optional[dict[str, str]] = None,
    ) -> Response:
        request_headers = dict(headers or {})
        request_headers.setdefault("Accept", "application/json")
        if self.cookie:
            request_headers["Cookie"] = self.cookie
        if method not in ("GET", "HEAD") and self.csrf:
            request_headers["X-CSRF-Token"] = self.csrf
            request_headers["Origin"] = "http://localhost"
        connection = http.client.HTTPConnection(self.host, self.port, timeout=5)
        connection.request(method, path, body, request_headers)
        raw = connection.getresponse()
        response = Response(raw.status, dict(raw.getheaders()), raw.read())
        set_cookie = response.headers.get("Set-Cookie")
        if set_cookie:
            self.cookie = set_cookie.split(";", 1)[0]
        connection.close()
        return response

    def json_request(self, method: str, path: str,
                     value: Optional[dict[str, Any]] = None) -> Response:
        encoded = b"" if value is None else json.dumps(
            value, separators=(",", ":")
        ).encode()
        return self.request(
            method, path, encoded,
            {"Content-Type": "application/json"} if value is not None else {},
        )

    def register_and_login(self, username: str) -> None:
        password = "correct-password"
        registered = self.json_request(
            "POST", "/api/auth/register",
            {"username": username, "password": password,
             "displayName": username},
        )
        if registered.status != 201:
            raise AssertionError(("register", registered.status,
                                  registered.body.decode(errors="replace")))
        payload = registered.json()
        user = payload.get("user", payload)
        self.user_id = int(user["id"])
        logged_in = self.json_request(
            "POST", "/api/auth/login",
            {"username": username, "password": password},
        )
        if logged_in.status != 200:
            raise AssertionError(("login", logged_in.status, logged_in.json()))
        self.csrf = logged_in.json()["csrfToken"]


def require_status(response: Response, expected: int, operation: str) -> Any:
    if response.status != expected:
        raise AssertionError((operation, response.status,
                              response.body.decode(errors="replace")))
    return response.json() if response.body else {}


def run(host: str, port: int) -> None:
    suffix = str(time.time_ns())[-9:]
    alice = Client(host, port)
    bob = Client(host, port)
    outsider = Client(host, port)
    alice.register_and_login("f17a" + suffix)
    bob.register_and_login("f17b" + suffix)
    outsider.register_and_login("f17c" + suffix)

    friend = require_status(
        alice.json_request("POST", "/api/friend-requests",
                           {"receiverId": str(bob.user_id)}),
        201, "friend request",
    )
    require_status(
        bob.json_request("POST", f"/api/friend-requests/{friend['id']}",
                         {"accept": "true"}),
        200, "accept friend request",
    )
    conversation = require_status(
        alice.json_request("POST", "/api/conversations",
                           {"kind": "direct", "peerId": str(bob.user_id)}),
        201, "direct conversation",
    )
    conversation_id = int(conversation["id"])

    # Small deterministic PNG-like payload: the boundary validates the signature;
    # media decoding is intentionally outside this storage service.
    content = b"\x89PNG\r\n\x1a\n" + b"phase17-blackbox-file-content"
    upload = require_status(
        alice.json_request("POST", "/api/files/uploads", {
            "name": "phase17.png",
            "mediaType": "image/png",
            "size": str(len(content)),
        }),
        201, "create upload",
    )
    file_id = int(upload["id"])

    first = content[:13]
    require_status(
        alice.request("PATCH", f"/api/files/{file_id}/content", first, {
            "Content-Type": "application/octet-stream", "Upload-Offset": "0",
        }),
        200, "first chunk",
    )
    require_status(
        alice.request("PATCH", f"/api/files/{file_id}/content", b"bad", {
            "Content-Type": "application/octet-stream", "Upload-Offset": "0",
        }),
        409, "wrong offset rejection",
    )
    require_status(
        alice.request("PATCH", f"/api/files/{file_id}/content", content[13:], {
            "Content-Type": "application/octet-stream",
            "Upload-Offset": str(len(first)),
        }),
        200, "second chunk",
    )
    completed = require_status(
        alice.json_request("POST", f"/api/files/{file_id}/complete"),
        200, "complete upload",
    )
    if completed["state"] != "ready" or len(completed["sha256"]) != 64:
        raise AssertionError(("completion metadata", completed))

    require_status(
        outsider.request("GET", f"/api/files/{file_id}"),
        403, "private pre-attachment access",
    )
    sent = require_status(
        alice.json_request(
            "POST", f"/api/conversations/{conversation_id}/file-messages", {
                "clientMessageId": "phase17-file-" + suffix,
                "fileId": str(file_id),
                "caption": "离线也能收到的图片",
            }),
        201, "file message",
    )
    attachments = sent["message"].get("attachments", [])
    if len(attachments) != 1 or int(attachments[0]["fileId"]) != file_id:
        raise AssertionError(("message attachment", sent))

    # Bob never opened WebSocket/SSE. Reading history and bytes now proves that
    # the message survives while the recipient is offline.
    history = require_status(
        bob.request("GET", f"/api/conversations/{conversation_id}/messages"
                    "?after=0&limit=20"),
        200, "offline history",
    )
    if not any(item.get("attachments") for item in history):
        raise AssertionError(("offline attachment missing", history))
    downloaded = bob.request("GET", f"/api/files/{file_id}")
    if downloaded.status != 200 or downloaded.body != content:
        raise AssertionError(("full download", downloaded.status,
                              downloaded.body))
    partial = bob.request("GET", f"/api/files/{file_id}", headers={
        "Range": "bytes=4-15",
    })
    if partial.status != 206 or partial.body != content[4:16]:
        raise AssertionError(("range download", partial.status, partial.body))

    require_status(
        alice.json_request("POST", "/api/me/avatar",
                           {"fileId": str(file_id)}),
        200, "set avatar",
    )
    public_avatar = outsider.request("GET", f"/api/files/{file_id}")
    if public_avatar.status != 200 or public_avatar.body != content:
        raise AssertionError(("authenticated avatar access",
                              public_avatar.status))

    print("PHASE17_FILE_BLACKBOX_OK", json.dumps({
        "alice": alice.user_id,
        "bob": bob.user_id,
        "outsider": outsider.user_id,
        "conversation": conversation_id,
        "file": file_id,
        "bytes": len(content),
        "rangeBytes": len(partial.body),
        "offlineDelivery": True,
    }, separators=(",", ":")))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    run(args.host, args.port)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
