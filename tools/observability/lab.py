"""Start and stop an isolated monitoring lab using explicitly provisioned binaries."""
import argparse
from contextlib import ExitStack
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts/project"))
from repro_runtime import browser_fixture, clean_environment, disposable_database, owned_server_process, require

PORTS = {"observer": 18785, "probe": 18786, "prometheus": 19090, "node": 19100,
         "collector": 14318, "tempo": 13200, "tempo_grpc": 19095, "tempo_ingest": 24318}
MIN_FREE = 2 * 1024**3
DATA_BUDGET = 1024**3


def private_write(path, text):
    with os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w", encoding="utf-8") as f:
        f.write(text)


def inputs(args):
    root = args.dependencies.resolve()
    candidates = {k: root / version / path for k, (version, path) in {
        "prometheus": ("prometheus-3.13.5", "prometheus"),
        "promtool": ("prometheus-3.13.5", "promtool"),
        "node": ("node_exporter-1.12.1", "node_exporter"),
        "collector": ("otelcol-0.165.0", "otelcol"),
        "tempo": ("tempo-3.0.3", "tempo"),
        "echarts": ("echarts-6.1.0", "echarts.min.js"),
    }.items()}
    absent = [str(p) for k, p in candidates.items() if not p.is_file() or k != "echarts" and not os.access(p, os.X_OK)]
    require(not absent, "Missing approved files:\n" + "\n".join(absent))
    return candidates


def local(service):
    return "127.0.0.1:" + str(PORTS[service])


def prepare_credentials(run):
    secret = secrets.token_hex(32)
    unsigned = f"v1.9000.{int(time.time()) + 2*86400}.ops"
    token = unsigned + "." + hmac.new(secret.encode(), unsigned.encode(), hashlib.sha256).hexdigest()
    private_write(run / "scrape-token", token)
    private_write(run / "token.key", secrets.token_hex(32))
    private_write(run / "producers.json", json.dumps({
        **{name + "-sim": {"domain": name, "simulated": True, "token": secrets.token_urlsafe(32)}
           for name in ("dag", "agent", "device")},
        "experiment-lab": {"domain": "experiment", "simulated": False, "token": secrets.token_urlsafe(32)},
    }))
    return secret


def prepare_configs(run, target):
    jobs = {**{k: local(k) for k in ("observer", "probe", "node", "prometheus")}, "nebula": f"127.0.0.1:{target}"}
    scrape = [{"job_name": k, "static_configs": [{"targets": [v]}],
               **({"authorization": {"type": "Bearer", "credentials_file": str(run / "scrape-token")}}
                  if k == "nebula" else {})} for k, v in jobs.items()]
    # JSON is a YAML subset and keeps generated paths safely escaped.
    private_write(run / "prometheus.yaml", json.dumps({
        "global": {"scrape_interval": "15s", "scrape_timeout": "6s", "evaluation_interval": "15s"},
        "rule_files": [str(run / "alerts.yaml")], "scrape_configs": scrape}))
    rules = [
        ("NebulaUnreachable", "nebula_probe_live == 0", "检查服务退出状态及最近故障标注。"),
        ("NebulaUnready", "nebula_probe_ready == 0 and nebula_probe_live == 1", "查看 PostgreSQL 连接池和业务就绪。"),
        ("NebulaScrapeFailed", 'up{job=~"nebula|probe|observer|node"} == 0', "核对数据源和运维凭据，检查探针是否正常。"),
        ("NebulaQueueBacklog", "webserver_phase11_db_executor_queued > 0", "对照聊天队列 P99 和连接池等待。"),
    ]
    private_write(run / "alerts.yaml", json.dumps({"groups": [{"name": "nebula", "rules": [
        {"alert": name, "expr": query, "for": "30s",
         "annotations": {"guide": guide, "window": "30s", "expression": query}} for name, query, guide in rules]}]}))
    private_write(run / "collector.yaml", json.dumps({
        "receivers": {"otlp": {"protocols": {"http": {"endpoint": local("collector")}}}},
        "processors": {"memory_limiter": {"check_interval":"1s","limit_mib":128,"spike_limit_mib":32},
                       "batch": {"timeout": "1s", "send_batch_size": 128, "send_batch_max_size": 256}},
        "exporters": {"otlp_http": {"endpoint": "http://" + local("tempo_ingest"),
                                   "sending_queue": {"queue_size": 32},
                                   "retry_on_failure": {"max_elapsed_time": "5s"}}},
        "service": {"pipelines": {"traces": {"receivers": ["otlp"],
                                            "processors": ["memory_limiter", "batch"], "exporters": ["otlp_http"]}}},
    }))
    private_write(run / "tempo.yaml", json.dumps({
        "target": "all",
        "server": {"http_listen_address": "127.0.0.1", "http_listen_port": PORTS["tempo"],
                   "grpc_listen_address": "127.0.0.1", "grpc_listen_port": PORTS["tempo_grpc"]},
        "distributor": {"receivers": {"otlp": {"protocols": {"http": {"endpoint": local("tempo_ingest")}}}}},
        "memberlist": {"bind_addr": ["127.0.0.1"]},
        "storage": {"trace": {"backend": "local", "wal": {"path": str(run / "tempo/wal")},
                              "local": {"path": str(run / "tempo/blocks")}}},
        "compaction": {"block_retention": "72h"},
        "usage_report": {"reporting_enabled": False}
    }))


def check_ports():
    with ExitStack() as sockets:
        for name in PORTS:
            sock = sockets.enter_context(socket.socket())
            try:
                sock.bind(("127.0.0.1", PORTS[name]))
            except OSError as error:
                raise RuntimeError("Monitoring port already in use: " + local(name)) from error


def start_process(stack, run, name, command):
    log = stack.enter_context((run / (name + ".log")).open("wb"))
    child = subprocess.Popen(list(map(str, command)), cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                             env=clean_environment())
    stack.callback(stop, child)
    return child


def stop(child):
    if child.poll() is None:
        child.terminate()
        try:
            child.wait(8)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()


def await_url(url):
    until = time.monotonic() + 35
    while time.monotonic() < until:
        try:
            with urllib.request.urlopen(url, timeout=2) as r:
                if r.status == 200:
                    return
        except OSError:
            time.sleep(.3)
    raise RuntimeError("No ready response: " + url)


def disk_used(path):
    return sum(item.stat().st_size for item in path.rglob("*") if item.is_file() and not item.is_symlink())


def health_sample(run, port, server, token, account):
    from .probe import probe_http
    status = {}
    for endpoint in ("live", "ready"):
        status[endpoint] = probe_http(port, "/health/" + endpoint)[0]
    code, body = probe_http(port, "/metrics", {"Authorization":"Bearer " + token})
    prefixes = (b"nebula_telemetry_dropped_total ", b"webserver_phase11_db_executor_queued ",
                b"nebula_telemetry_exported_total ")
    status["metrics"] = code
    status["counters"] = {raw.split()[0].decode(): int(raw.split()[1]) for raw in body.splitlines() if raw.startswith(prefixes)}
    proc = Path("/proc") / str(server.pid)
    status["rssKiB"] = next(int(line.split()[1]) for line in (proc/"status").read_text().splitlines()
                            if line.startswith("VmRSS:"))
    status["descriptors"] = len(list((proc/"fd").iterdir()))
    status["diskBytes"] = disk_used(run)
    if account is not None:
        user, username, password = account
        code, body = user.request("GET", "/api/conversations")
        if code == 401:
            login, _ = user.request("POST", "/api/auth/login", {"username":username,"password":password})
            require(login == 200, "Synthetic session could not be renewed")
            code, body = user.request("GET", "/api/conversations")
        status["syntheticHttp"] = code
        if code != 200 or not isinstance(body, list):
            raise RuntimeError("Synthetic conversation read failed")
    status["timestamp"] = int(time.time())
    with (run / "soak.ndjson").open("a", encoding="utf-8") as log:
        log.write(json.dumps(status) + "\n")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--work-dir", type=Path, required=True)
    p.add_argument("--dependencies", type=Path, required=True)
    p.add_argument("--postgres-prefix", type=Path, required=True)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--duration", type=int, default=3600, metavar="SECONDS")
    p.add_argument("--exercise", action="store_true", help="Read conversations once per minute as an isolated synthetic account")
    args = p.parse_args()
    require(sys.platform.startswith("linux") and os.geteuid() > 0, "Use an unprivileged Linux account")
    require(1 <= args.duration <= 86400, "Duration must be 1–86400 seconds")
    binaries = inputs(args)
    check_ports()
    run = args.work_dir.resolve()
    run.mkdir(mode=0o700, parents=True, exist_ok=False)
    os.umask(0o077)
    require(shutil.disk_usage(run).free > MIN_FREE, "Not enough free disk for monitoring lab")
    done = threading.Event()
    for name in (signal.SIGINT, signal.SIGTERM):
        signal.signal(name, lambda *_: done.set())
    # The normal NEBULA runner allocates a disposable TCP port and owns the child.
    with disposable_database(args.postgres_prefix.resolve(), ROOT, run) as (connection, migration):
        env = {**clean_environment(), "WEB_PHASE11_REQUIRED": "1", "WEB_FUNCTION3_ENABLE": "1",
               "WEB_PHASE11_DATABASE": connection, "WEB_PHASE11_TOKEN_KEY_FILE": str(run / "token.key"),
               "WEB_PHASE11_FILE_ROOT": str(run / "files"), "WEB_FUNCTION3_MEDIA_ROOT": str(run / "media"),
               "WEB_OBSERVABILITY": "1", "WEB_OBSERVABILITY_LOG": "error",
               "WEB_OBSERVABILITY_OTLP": "http://" + local("collector")}
        # Prepare the server key before the ephemeral target port is known.
        env["WEB_AUTH_SECRET"] = prepare_credentials(run)
        with owned_server_process(args.binary.resolve(), ROOT, env, run / "nebula.log") as (target, server), ExitStack() as stack:
            prepare_configs(run, target)
            subprocess.run([str(binaries["promtool"]), "check", "config", str(run / "prometheus.yaml")],
                           timeout=10, check=True, env=clean_environment())
            commands = {
                "tempo": [binaries["tempo"], "-config.file=" + str(run / "tempo.yaml")],
                "collector": [binaries["collector"], "--config=" + str(run / "collector.yaml")],
                "node": [binaries["node"], "--web.listen-address=" + local("node")],
                "probe": [sys.executable, "-m", "tools.observability.probe", "serve", "--target-port", target, "--port", PORTS["probe"]],
                "observer": [sys.executable, "-m", "tools.observability.observer.server", "serve",
                             "--db", run / "events.sqlite", "--credentials", run / "producers.json",
                             "--echarts", binaries["echarts"], "--port", PORTS["observer"]],
                "prometheus": [binaries["prometheus"], "--config.file=" + str(run / "prometheus.yaml"),
                               "--web.listen-address=" + local("prometheus"), "--storage.tsdb.path=" + str(run / "prometheus"),
                               "--storage.tsdb.retention.time=7d", "--storage.tsdb.retention.size=384MB"],
            }
            processes = {name: start_process(stack, run, name, cmd) for name, cmd in commands.items()}
            await_url("http://" + local("observer") + "/health")
            await_url("http://" + local("prometheus") + "/-/ready")
            await_url("http://" + local("tempo") + "/ready")
            account = None
            if args.exercise:
                user = browser_fixture(ROOT).BrowserClient("127.0.0.1", target)
                username, password = "obs_soak_" + secrets.token_hex(6), secrets.token_urlsafe(20)
                user.register_and_login(username, password)
                account = user, username, password
            private_write(run / "session.json", json.dumps({"nebulaPort": target, "migration": migration, "ports": PORTS}))
            print(json.dumps({"observer": "http://" + local("observer"), "target": target}), flush=True)
            start = time.monotonic()
            last_sample = start - 60
            scrape_token = (run / "scrape-token").read_text()
            while not done.wait(2):
                dead = [name for name, child in processes.items() if child.poll() is not None]
                require(not dead, "Monitoring component exited: " + ", ".join(dead))
                require(server.poll() is None, "Monitored NEBULA process exited")
                if time.monotonic() - start >= args.duration:
                    break
                require(disk_used(run) < DATA_BUDGET and shutil.disk_usage(run).free > 1024**3,
                        "Monitoring lab reached its disk budget; stopping gracefully")
                if time.monotonic() - last_sample >= 60:
                    health_sample(run, target, server, scrape_token, account)
                    last_sample = time.monotonic()


if __name__ == "__main__":
    main()
