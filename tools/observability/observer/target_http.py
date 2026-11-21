"""Bounded HTTP connections to operator-approved monitoring targets."""
import concurrent.futures
import http.client
import ipaddress
import json
import re
import socket
import time
import threading
from urllib.parse import urlencode, urlsplit, urlunsplit

from .events import Invalid
from .sources import BackendError

MAX_RESPONSE = 1024 * 1024
DENIED = tuple(map(ipaddress.ip_network, ("169.254.0.0/16", "100.100.100.200/32",
                                         "168.63.129.16/32", "fd00:ec2::254/128")))

# Keep DNS work bounded even if the system resolver stalls.
DNS_POOL = concurrent.futures.ThreadPoolExecutor(max_workers=2)
DNS_SLOTS = threading.BoundedSemaphore(4)


def clean_url(raw):
    if (not isinstance(raw, str) or not 1 <= len(raw) <= 512
            or any(ord(c) <= 32 or ord(c) >= 127 for c in raw) or "\\" in raw or "%" in raw):
        raise Invalid("请输入无空格的 HTTP(S) URL")
    url = urlsplit(raw)
    if (url.scheme not in ("http", "https") or not url.hostname or url.username is not None
            or url.query or url.fragment or "?" in raw or "#" in raw
            or not re.fullmatch(r"[A-Za-z0-9.:\-]+", url.hostname)
            or ".." in url.path.split("/") or url.port == 0):
        raise Invalid("URL 需包含主机，且不能带凭据、参数或片段")
    return urlunsplit((url.scheme, url.netloc.lower(), url.path or "/", "", ""))


class TargetHttp:
    def __init__(self, networks=(), observer_port=18785, auth=None):
        self.networks = tuple(map(ipaddress.ip_network, networks))
        self.observer_port = observer_port
        self.auth = auth or {}

    def permitted(self, addr, port):
        if isinstance(addr, ipaddress.IPv6Address) and addr.ipv4_mapped:
            addr=addr.ipv4_mapped
        if addr.is_unspecified or addr.is_multicast or addr.is_link_local or any(addr in n for n in DENIED):
            return False
        if addr.is_loopback:
            return port not in (self.observer_port, 22)
        return any(addr in net for net in self.networks)

    def addresses(self, host, port):
        if host == "localhost":
            return [ipaddress.ip_address("127.0.0.1")]
        try:
            address = ipaddress.ip_address(host)
        except ValueError:
            if not self.networks:
                raise Invalid("此实例仅允许连接回环地址，远端网段需由运维配置")
            if not DNS_SLOTS.acquire(blocking=False):
                raise BackendError("数据源解析繁忙")
            future = DNS_POOL.submit(socket.getaddrinfo, host, port, 0, socket.SOCK_STREAM)
            future.add_done_callback(lambda _: DNS_SLOTS.release())
            try:
                return [ipaddress.ip_address(r[4][0]) for r in future.result(timeout=1.5)]
            except (OSError, concurrent.futures.TimeoutError) as error:
                raise BackendError("数据源域名解析超时或失败") from error
        if isinstance(address, ipaddress.IPv6Address) and address.ipv4_mapped:
            address = address.ipv4_mapped
        return [address]

    def get(self, base, suffix="", params=None, token=None, read_body=True, payload=None):
        base=clean_url(base)
        url = urlsplit(base)
        port = url.port or (443 if url.scheme == "https" else 80)
        addresses = self.addresses(url.hostname, port)
        if not addresses or any(not self.permitted(ip, port) for ip in addresses):
            raise Invalid("目标不在允许的监控网段内，或指向保留地址")
        endpoint = (url.path.rstrip("/") + suffix if suffix else url.path) or "/"
        if params:
            endpoint += "?" + urlencode(params)
        client = (http.client.HTTPSConnection if url.scheme == "https" else http.client.HTTPConnection)(url.hostname, port, timeout=2)
        deadline = time.monotonic()+3
        # Connect to the address we checked. HTTPS still verifies the URL hostname.
        client._create_connection = lambda _addr, timeout, source: socket.create_connection((str(addresses[0]), port), timeout, source)
        try:
            headers=self.auth.get(base,{})
            if token:
                headers={**headers,"Authorization":"Bearer "+token}
            if headers and url.scheme!="https" and not all(ip.is_loopback for ip in addresses):
                raise Invalid("带凭据的远端目标需要 HTTPS")
            if payload is None:
                client.request("GET", endpoint, headers=headers)
            else:
                client.request("POST",endpoint,body=json.dumps(payload).encode(),headers={
                    **headers,"Content-Type":"application/json","Connect-Protocol-Version":"1"})
            reply = client.getresponse()
            if not read_body or reply.status != 200:
                return reply.status, b""
            chunks, remaining = [], MAX_RESPONSE+1
            while remaining:
                left = deadline-time.monotonic()
                if left <= 0:
                    raise TimeoutError
                if client.sock:
                    client.sock.settimeout(min(left,2))
                chunk = reply.read1(min(16384,remaining))
                if not chunk:
                    break
                remaining -= len(chunk)
                chunks.append(chunk)
            if not remaining:
                raise BackendError("数据源响应超过 1 MiB")
            return reply.status, b"".join(chunks)
        except (OSError, http.client.HTTPException) as error:
            raise BackendError("无法连接数据源，或查询超时") from error
        finally:
            client.close()

    def prom(self, base, suffix, params):
        status, body = self.get(base, "/api/v1/" + suffix, params)
        if status != 200 and status not in (400, 422):
            raise BackendError(f"Prometheus 返回 HTTP {status}")
        if status in (400, 422):
            raise Invalid("Prometheus 拒绝查询，请检查 PromQL")
        try:
            value = json.loads(body)
            if value["status"] == "success" and isinstance(value["data"],(dict,list)):
                return value["data"]
        except (ValueError, TypeError, KeyError):
            pass
        raise BackendError("目标未返回有效的 Prometheus API 数据")


def read_auth(path):
    config=json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(config,dict) or len(config)>8:
        raise Invalid("最多配置 8 组连接凭据")
    result={}
    for url,row in config.items():
        if not isinstance(row,dict) or not row.keys() <= {"bearer_file","tenant"}:
            raise Invalid("未知的连接凭据字段")
        headers={}
        if "bearer_file" in row:
            secret=path.parent.joinpath(row["bearer_file"]).read_text(encoding="utf-8").strip()
            if not re.fullmatch(r"[\x21-\x7e]{1,2048}",secret):
                raise Invalid("数据源令牌格式无效")
            headers["Authorization"]="Bearer "+secret
        if "tenant" in row:
            if not isinstance(row["tenant"],str) or not re.fullmatch(r"[a-zA-Z0-9._-]{1,64}",row["tenant"]):
                raise Invalid("数据源租户无效")
            headers["X-Scope-OrgID"]=row["tenant"]
        result[clean_url(url)]=headers
    return result
