#!/usr/bin/env python3
"""判断 / 释放本仓库 webserver 对 TCP 8080 的【监听】占用。

注意：黑盒刚结束后，大量连接会进入 TIME_WAIT。此时 ss 看不到 LISTEN，
但「不带 SO_REUSEADDR 的 bind」仍会 EADDRINUSE —— 这是误报。
本仓库服务器启动时会设 SO_REUSEADDR，因此探测与清理都按「是否存在 LISTEN」为准。
"""

from __future__ import annotations

import os
import re
import signal
import socket
import subprocess
import sys
import time

PORT = 8080


def listen_inodes(port: int) -> set[str]:
    """/proc/net/tcp(6) 中状态为 LISTEN(0A) 且本地端口匹配的 inode。"""
    inodes: set[str] = set()
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
            if local_port == port and parts[9] != "0":
                inodes.add(parts[9])
    return inodes


def ss_has_listen(port: int) -> bool:
    try:
        out = subprocess.check_output(
            ["ss", "-ltn"], text=True, stderr=subprocess.DEVNULL
        )
    except (OSError, subprocess.CalledProcessError):
        return False
    # 匹配 :8080 且行内含 Listen（ss 默认英文）
    for line in out.splitlines():
        if f":{port}" in line and "LISTEN" in line.upper():
            return True
    return False


def has_listener(port: int) -> bool:
    return bool(listen_inodes(port)) or ss_has_listen(port)


def can_bind_like_server(port: int) -> bool:
    """与服务器一致：SO_REUSEADDR 后能否 bind（TIME_WAIT 不应挡启动）。"""
    sock = socket.socket()
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", port))
        return True
    except OSError:
        return False
    finally:
        sock.close()


def port_ready(port: int) -> bool:
    """无 LISTEN，或虽短暂异常但带 REUSEADDR 仍可 bind → 视为可启动。"""
    if not has_listener(port):
        return True
    return can_bind_like_server(port)


def cmdline(pid: int) -> str:
    try:
        raw = open(f"/proc/{pid}/cmdline", "rb").read()
    except OSError:
        return ""
    return raw.replace(b"\0", b" ").decode("utf-8", "replace")


def pids_holding_inodes(inodes: set[str]) -> set[int]:
    found: set[int] = set()
    if not inodes:
        return found
    try:
        entries = os.listdir("/proc")
    except OSError:
        return found
    for entry in entries:
        if not entry.isdigit():
            continue
        pid = int(entry)
        fd_dir = f"/proc/{pid}/fd"
        try:
            fds = os.listdir(fd_dir)
        except OSError:
            continue
        for fd in fds:
            try:
                target = os.readlink(f"{fd_dir}/{fd}")
            except OSError:
                continue
            if target.startswith("socket:[") and target[8:-1] in inodes:
                found.add(pid)
                break
    return found


def pids_via_ss(port: int) -> set[int]:
    try:
        out = subprocess.check_output(
            ["ss", "-ltnp"], text=True, stderr=subprocess.DEVNULL
        )
    except (OSError, subprocess.CalledProcessError):
        return set()
    pids: set[int] = set()
    for line in out.splitlines():
        if f":{port}" not in line:
            continue
        pids.update(int(x) for x in re.findall(r"pid=(\d+)", line))
    return pids


def collect_listener_pids(port: int) -> set[int]:
    return pids_via_ss(port) | pids_holding_inodes(listen_inodes(port))


def is_ours(pid: int, hint: str) -> bool:
    cmd = cmdline(pid)
    base = os.path.basename(hint)
    return bool(cmd) and (
        hint in cmd or f"/{base}" in cmd or cmd.strip().endswith(base)
    )


def pkill_hint(hint: str, sig: int) -> None:
    for pattern in (hint, os.path.basename(hint)):
        try:
            subprocess.run(
                ["pkill", f"-{sig}", "-f", pattern],
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except OSError:
            pass


def wait_ready(seconds: float) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if port_ready(PORT):
            # 无 LISTEN 即可；TIME_WAIT 可忽略
            if not has_listener(PORT):
                return True
            if can_bind_like_server(PORT):
                return True
        time.sleep(0.1)
    return port_ready(PORT) and not has_listener(PORT)


def main() -> int:
    hint = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else "webserver"

    # 无监听进程：即使 TIME_WAIT 仍在，也对压测/黑盒放行
    if not has_listener(PORT):
        return 0

    killed: list[str] = []
    for round_i, sig in enumerate((signal.SIGTERM, signal.SIGKILL)):
        for pid in sorted(collect_listener_pids(PORT)):
            if not is_ours(pid, hint) and "webserver" not in cmdline(pid):
                if round_i == 0:
                    continue
            try:
                os.kill(pid, sig)
                killed.append(f"{pid}:{signal.Signals(sig).name}")
            except ProcessLookupError:
                pass
            except PermissionError:
                killed.append(f"{pid}:EPERM")
        pkill_hint(hint, int(sig))
        if wait_ready(5.0 if round_i == 0 else 3.0):
            msg = ", ".join(killed) if killed else "wait/pkill"
            print(f"freed :{PORT} listen (killed {msg})", flush=True)
            return 0

    listeners = sorted(collect_listener_pids(PORT))
    details = [f"{pid}:{cmdline(pid)!r}" for pid in listeners]
    print(
        f"port :{PORT} still has LISTEN; pids={listeners}; "
        f"inodes={sorted(listen_inodes(PORT))}; detail={details}",
        flush=True,
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
