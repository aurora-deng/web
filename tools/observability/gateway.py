"""Render and own a loopback Envoy gateway for a disposable NEBULA lab."""

from contextlib import contextmanager
from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
TEMPLATE = ROOT / "config/gateway/envoy.json"
PROTECTED_PORTS = frozenset({18090, 18443, 18791, 5432, 6379, 50051})
DATA_BUDGET = 128 * 1024**2
RATE_LIMIT = {
    "@type": "type.googleapis.com/envoy.extensions.filters.http.local_ratelimit.v3.LocalRateLimit",
    "stat_prefix": "nebula_auth",
    "token_bucket": {"max_tokens": 16, "tokens_per_fill": 5, "fill_interval": "1s"},
    "filter_enabled": {"default_value": {"numerator": 100, "denominator": "HUNDRED"}},
    "filter_enforced": {"default_value": {"numerator": 100, "denominator": "HUNDRED"}},
}


def port_ok(port):
    return type(port) is int and 1024 <= port <= 65535 and port not in PROTECTED_PORTS


def options(args):
    if not args.gateway_envoy:
        if args.gateway_https_port or args.gateway_cert or args.gateway_key or args.gateway_wasm or args.gateway_wasm_sha256:
            raise ValueError("Gateway TLS or Wasm requires --gateway-envoy")
        return None
    binary = args.gateway_envoy.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError("Specify an existing executable Envoy binary")
    ports = [args.gateway_port, args.gateway_admin_port]
    if args.gateway_https_port is not None:
        ports.append(args.gateway_https_port)
    check_ports(ports)
    tls = bool(args.gateway_https_port)
    if tls != bool(args.gateway_cert) or tls != bool(args.gateway_key):
        raise ValueError("TLS requires the HTTPS port, certificate and private key together")
    if tls and not args.gateway_cert.resolve().is_file():
        raise ValueError("Gateway certificate file is missing")
    if tls:
        from .observer.access_preflight import private_file
        private_file(args.gateway_key)
    if bool(args.gateway_wasm) != bool(args.gateway_wasm_sha256):
        raise ValueError("Wasm requires the module path and SHA-256 together")
    if args.gateway_wasm:
        verified_wasm(args.gateway_wasm, args.gateway_wasm_sha256)
    return binary


def check_ports(ports):
    if not all(map(port_ok, ports)) or len(set(ports)) != len(ports):
        raise ValueError("Gateway, admin and upstream must use distinct unprivileged lab ports")


def verified_wasm(path, expected):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 2 * 1024**2:
        raise ValueError("Use an explicit regular Wasm file of at most 2 MiB")
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise ValueError("Wasm SHA-256 must be 64 lowercase hex digits")
    with path.open("rb") as stream:
        contents = stream.read(2 * 1024**2 + 1)
    if not contents.startswith(b"\0asm\x01\0\0\0") or hashlib.sha256(contents).hexdigest() != expected:
        raise ValueError("Wasm artifact header or SHA-256 mismatch")
    return contents


def address(port):
    return {"socket_address": {"address": "127.0.0.1", "port_value": port}}


def stream_routes():
    websocket = {"cluster": "nebula_application", "timeout": "0s", "idle_timeout": "90s",
                 "upgrade_configs": [{"upgrade_type": "websocket", "enabled": True}]}
    sse = {"cluster": "nebula_application", "timeout": "0s", "idle_timeout": "90s",
           "upgrade_configs": [{"upgrade_type": "websocket", "enabled": False}]}
    return [{"name": "ws", "match": {"path": "/ws"}, "route": websocket},
            {"name": "sse", "match": {"path": "/events"}, "route": sse}]


def routes():
    result = stream_routes()
    ordinary = (("auth_login", "/api/auth/login"), ("auth_register", "/api/auth/register"),
                ("conversations", "/api/conversations"), ("tree", "/api/tree/posts"),
                ("health_live", "/health/live"), ("health_ready", "/health/ready"),
                ("health_version", "/health/version"), ("fast", "/fast"))
    for name, path in ordinary:
        route = proxy_route(name, {"path": path})
        if name.startswith("auth_"):
            route["typed_per_filter_config"] = {"envoy.filters.http.local_ratelimit": deepcopy(RATE_LIMIT)}
        result.append(route)
    result.extend((proxy_route("files", {"prefix": "/api/files/"}, "45s"),
                   proxy_route("api", {"prefix": "/api/"}), proxy_route("page", {"prefix": "/phase11/"})))
    result.append({"name": "deny", "match": {"prefix": "/"}, "direct_response": {"status": 403}})
    return result


def proxy_route(name, match, timeout="15s"):
    return {"name": name, "match": match, "route": {"cluster": "nebula_application", "timeout": timeout,
            "upgrade_configs": [{"upgrade_type": "websocket", "enabled": False}]}}


def tls_socket(cert, key):
    return {"name": "envoy.transport_sockets.tls", "typed_config": {
        "@type": "type.googleapis.com/envoy.extensions.transport_sockets.tls.v3.DownstreamTlsContext",
        "common_tls_context": {
            "tls_params": {"tls_minimum_protocol_version": "TLSv1_2"},
            "alpn_protocols": ["h2", "http/1.1"],
            "tls_certificates": [{"certificate_chain": {"filename": str(cert.resolve())},
                                  "private_key": {"filename": str(key.resolve())}}]}}}


def wasm_filter(path):
    return {"name": "envoy.filters.http.wasm", "typed_config": {
        "@type": "type.googleapis.com/envoy.extensions.filters.http.wasm.v3.Wasm",
        "config": {"name": "nebula_stream_probe", "root_id": "nebula_stream_probe",
                   "failure_policy": "FAIL_OPEN",
                   "vm_config": {"vm_id": "nebula_stream_probe", "runtime": "envoy.wasm.runtime.v8",
                                 "code": {"local": {"filename": str(path.resolve())}}}}}}


def render(gateway_port, admin_port, upstream_port, log_path, *, https_port=None, cert=None, key=None,
           wasm=None, owns_upstream=True, replica_port=None):
    ports = [gateway_port, admin_port] + ([https_port] if https_port is not None else [])
    upstreams = [upstream_port]
    if replica_port is not None:
        if not owns_upstream:
            raise ValueError("The replica must belong to the same isolated lab")
        upstreams.append(replica_port)
    check_ports([*ports, *upstreams] if owns_upstream else ports)
    if not owns_upstream and (not 0 < upstream_port < 65536 or upstream_port in ports):
        raise ValueError('The existing upstream must use a separate valid port')
    config = json.loads(TEMPLATE.read_text(encoding="utf-8"))
    config["admin"]["address"] = address(admin_port)
    static = config["static_resources"]
    listener = static["listeners"][0]
    listener["address"] = address(gateway_port)
    chain = listener["filter_chains"][0]
    http = chain["filters"][0]["typed_config"]
    http["route_config"]["virtual_hosts"][0]["routes"] = routes()
    http["access_log"][0]["typed_config"]["path"] = str(log_path.resolve())
    if wasm:
        http["http_filters"].insert(-1, wasm_filter(wasm))
    if https_port is not None:
        if not cert or not key:
            raise ValueError("TLS requires a certificate and private key")
        tls = deepcopy(listener)
        tls["name"] = "nebula_https"
        tls["address"] = address(https_port)
        tls["filter_chains"][0]["transport_socket"] = tls_socket(cert, key)
        static["listeners"].append(tls)
    application = static["clusters"][0]
    application["load_assignment"] = {"cluster_name": "nebula_application", "endpoints": [
        {"lb_endpoints": [{"endpoint": {"address": address(port)}} for port in upstreams]}]}
    if replica_port is not None:
        application.update(lb_policy="ROUND_ROBIN", common_lb_config={
            "healthy_panic_threshold": {"value": 0}, "ignore_new_hosts_until_first_hc": True,
        }, health_checks=[{
            "timeout": "1s", "interval": "1s", "no_traffic_interval": "1s",
            "healthy_threshold": 1, "unhealthy_threshold": 2,
            "http_health_check": {"path": "/health/ready"},
        }])
    return config


@contextmanager
def start(binary, directory, args, upstream_port, *, replica_port=None):
    if directory.is_symlink():
        raise ValueError("Gateway lab directory must not be a symbolic link")
    directory.mkdir(mode=0o700, exist_ok=True)
    config = directory / "envoy.json"
    access_log = directory / "access.jsonl"
    stderr = directory / "envoy-stderr.log"
    module = directory / "stream-probe.wasm"
    if any(path.is_symlink() for path in (config, access_log, stderr, module)):
        raise ValueError("Gateway lab files must not be symbolic links")
    if args.gateway_wasm:
        module.write_bytes(verified_wasm(args.gateway_wasm, args.gateway_wasm_sha256))
    bootstrap = render(args.gateway_port, args.gateway_admin_port, upstream_port, access_log,
                       https_port=args.gateway_https_port, cert=args.gateway_cert, key=args.gateway_key,
                       wasm=module if args.gateway_wasm else None, replica_port=replica_port)
    config.write_text(json.dumps(bootstrap, indent=2) + "\n", encoding="utf-8")
    command = [str(binary), "-c", str(config), "--disable-hot-restart", "--concurrency", "2", "-l", "warning"]
    with stderr.open("ab") as log:
        subprocess.run([*command, "--mode", "validate"], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=15)
        child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            yield child
        finally:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=3)
