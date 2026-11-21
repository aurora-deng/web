"""Control only this application's own Cloudflare tunnel process."""

import argparse
import ctypes
import json
import os
import re
import subprocess
import threading
import time
import secrets
import base64
import http.client
import select
import socketserver
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib import request as url_request
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parent
BINARY = ROOT / "tools" / "cloudflared.exe"
NAMED_BINARY = ROOT / "pc" / "yantu_pc_002.exe"
LOCK = threading.RLock()
BRIDGES = {}


def edge_proxy_bridge(proxy):
    address = urlsplit(proxy)
    if address.scheme not in {"http", "https"} or not address.hostname:
        return None

    class Bridge(socketserver.ThreadingTCPServer):
        allow_reuse_address = True
        daemon_threads = True

    class Forward(socketserver.BaseRequestHandler):
        def handle(self):
            connection = None
            try:
                cls = http.client.HTTPSConnection if address.scheme == "https" else http.client.HTTPConnection
                connection = cls(address.hostname, address.port or (443 if address.scheme == "https" else 80), timeout=15)
                headers = {}
                if address.username:
                    credentials = f"{unquote(address.username)}:{unquote(address.password or '')}"
                    headers["Proxy-Authorization"] = "Basic " + base64.b64encode(credentials.encode()).decode()
                connection.set_tunnel("region1.v2.argotunnel.com", 7844, headers=headers)
                connection.connect()
                upstream = connection.sock
                upstream.settimeout(None)
                self.request.settimeout(None)
                while not self.server.stopping.is_set():
                    readable, _, _ = select.select([self.request, upstream], [], [], 1)
                    for source in readable:
                        chunk = source.recv(65536)
                        if not chunk:
                            return
                        (upstream if source is self.request else self.request).sendall(chunk)
            except (OSError, http.client.HTTPException):
                pass
            finally:
                if connection:
                    connection.close()

    bridge = Bridge(("127.0.0.1", 0), Forward)
    bridge.stopping = threading.Event()
    threading.Thread(target=bridge.serve_forever, daemon=True).start()
    return bridge


def close_bridge(pid):
    bridge = BRIDGES.pop(pid, None)
    if bridge:
        bridge.stopping.set()
        bridge.shutdown()
        bridge.server_close()


def system_proxies():
    proxies = url_request.getproxies()
    if os.name == "nt":
        proxies = {**url_request.getproxies_registry(), **proxies}
    return proxies


def quick_service_relay(proxies):
    # The official quick-service flag lets provisioning use the existing system proxy.
    token = secrets.token_hex(12)
    opener = url_request.build_opener(url_request.ProxyHandler(proxies))

    class Relay(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            if self.path != f"/{token}/tunnel":
                self.send_error(404)
                return
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 <= length <= 2048:
                self.send_error(400)
                return
            payload = self.rfile.read(length)
            request = url_request.Request("https://api.trycloudflare.com/tunnel", data=payload, method="POST",
                                          headers={"Content-Type": "application/json", "User-Agent": self.headers.get("User-Agent", "cloudflared")})
            try:
                with opener.open(request, timeout=20) as response:
                    body = response.read(1024 * 1024)
                    self.send_response(response.status)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
            except Exception:
                self.send_error(502, "Cloudflare provisioning is not reachable through the system proxy")

    relay = ThreadingHTTPServer(("127.0.0.1", 0), Relay)
    relay.daemon_threads = True
    threading.Thread(target=relay.serve_forever, daemon=True).start()
    return relay, f"http://127.0.0.1:{relay.server_port}/{token}"


def write_status(data_dir, value):
    path = data_dir / "remote.json"
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
    temporary.replace(path)


def owned_process(pid):
    if os.name != "nt" or not pid:
        return False
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    kernel.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint32)]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.OpenProcess(0x1000, False, int(pid))
    if not handle:
        return False
    try:
        buffer = ctypes.create_unicode_buffer(32768)
        size = ctypes.c_uint32(len(buffer))
        return bool(kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size))) and Path(buffer.value).resolve() in {BINARY.resolve(), NAMED_BINARY.resolve()}
    finally:
        kernel.CloseHandle(handle)


def remote_status(data_dir):
    path = data_dir / "remote.json"
    if not path.exists():
        return {"active": False}
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        value["active"] = bool(value.get("url") and owned_process(value.get("pid")))
        if value.get("bridgeOwner"):
            runtime = json.loads((data_dir / "runtime.json").read_text(encoding="utf-8"))
            value["active"] = value["active"] and runtime.get("pid") == value["bridgeOwner"]
        return value
    except (OSError, ValueError):
        return {"active": False}


def stop_remote(data_dir):
    with LOCK:
        status = remote_status(data_dir)
        pid = status.get("pid")
        close_bridge(pid)
        if owned_process(pid):
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
            kernel.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            handle = kernel.OpenProcess(1, False, pid)
            if not handle:
                return {"active": True, "error": "无法停止隧道进程"}
            try:
                if not kernel.TerminateProcess(handle, 0):
                    return {"active": True, "error": "隧道进程停止失败"}
            finally:
                kernel.CloseHandle(handle)
        write_status(data_dir, {"active": False})
        return {"active": False}


def start_remote(data_dir, port=8765):
    with LOCK:
        current = remote_status(data_dir)
        if current.get("active"):
            return current
        if owned_process(current.get("pid")):
            stop_remote(data_dir)
        binary = NAMED_BINARY if NAMED_BINARY.is_file() else BINARY
        if not binary.is_file():
            return {"active": False, "error": "先运行 Enable-Remote.cmd 下载远程连接组件"}
        data_dir.mkdir(parents=True, exist_ok=True)
        log = data_dir / "remote-tunnel.log"
        environment = dict(os.environ)
        proxies = system_proxies()
        for scheme in ("http", "https"):
            if proxies.get(scheme):
                environment.setdefault(f"{scheme.upper()}_PROXY", proxies[scheme])
        relay = None
        bridge = edge_proxy_bridge(proxies.get("https") or proxies.get("http") or "")
        arguments = [str(binary), "tunnel", "--no-autoupdate", "--protocol", "http2" if bridge else "quic", "--edge-ip-version", "4", "--url", f"http://127.0.0.1:{port}"]
        if bridge:
            arguments.extend(["--edge", f"127.0.0.1:{bridge.server_address[1]}"])
        if proxies.get("https") or proxies.get("http"):
            relay, service_url = quick_service_relay(proxies)
            arguments.extend(["--quick-service", service_url])
        with log.open("wb") as output:
            process = subprocess.Popen(arguments,
                                       stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW, env=environment)
        status = {"pid": process.pid, "port": port, "active": False, "started": time.time()}
        if bridge:
            BRIDGES[process.pid] = bridge
            status["bridgeOwner"] = os.getpid()
        write_status(data_dir, status)
        # Quick tunnels publish their assigned URL before connecting to the edge.
        for _ in range(110):
            text = log.read_text(encoding="utf-8", errors="replace")
            match = re.search(r"https://[a-z0-9-]+\.trycloudflare\.com", text)
            if match and "Registered tunnel connection" in text:
                status.update({"active": True, "url": match.group(0)})
                write_status(data_dir, status)
                if relay:
                    relay.shutdown()
                    relay.server_close()
                return status
            if process.poll() is not None:
                status["error"] = "远程连接未建立，请查看 remote-tunnel.log"
                break
            time.sleep(0.5)
        else:
            status["error"] = "远程连接超时，请检查互联网连接后重试"
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
        if relay:
            relay.shutdown()
            relay.server_close()
        close_bridge(process.pid)
        status["active"] = False
        write_status(data_dir, status)
        return status


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=["start", "stop", "status"])
    parser.add_argument("--data-dir", required=True)
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    data_dir = Path(args.data_dir).resolve()
    result = start_remote(data_dir, args.port) if args.action == "start" else stop_remote(data_dir) if args.action == "stop" else remote_status(data_dir)
    print(json.dumps(result, ensure_ascii=False))
    if result.get("error"):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
