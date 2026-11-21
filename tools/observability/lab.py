"""Start and stop an isolated monitoring lab using explicitly provisioned binaries."""
import argparse
from contextlib import ExitStack
import hashlib
import hmac
import http.client
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import threading
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts/project"))
from repro_runtime import browser_fixture, clean_environment, disposable_database, owned_server_process, require
from . import gateway, lab_config
from .isolated_dual import require_isolated_network

PORTS = {"observer": 18785, "probe": 18786, "prometheus": 19090, "node": 19100,
         "collector": 14318, "tempo": 13200, "tempo_grpc": 19095, "tempo_ingest": 24318,
         "collector_metrics": 18888}
MIB = 1024**2
DISK_RESERVE = 1024 * MIB


def private_write(path, text):
    temporary=path.with_name(path.name+".tmp-"+secrets.token_hex(4))
    try:
        with os.fdopen(os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w", encoding="utf-8") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.replace(temporary,path)
    finally:
        temporary.unlink(missing_ok=True)


def inputs(args):
    root = args.dependencies.resolve()
    candidates = {k: root / version / path for k, (version, path) in {
        "prometheus": ("prometheus-3.13.5", "prometheus"),
        "promtool": ("prometheus-3.13.5", "promtool"),
        "node": ("node_exporter-1.12.1", "node_exporter"),
        "collector": ("otelcol-0.165.0", "otelcol"),
        "tempo": ("tempo-3.0.3", "tempo"),
    }.items()}
    if args.profile=='metrics':
        candidates={key:value for key,value in candidates.items() if key in ('prometheus','promtool','node')}
    if args.with_alertmanager:
        candidates.update({name: root/"alertmanager-0.34.2"/name for name in ("alertmanager", "amtool")})
    absent = [str(p) for p in candidates.values() if not p.is_file() or not os.access(p, os.X_OK)]
    require(not absent, "Missing approved files:\n" + "\n".join(absent))
    return candidates


def local(service):
    return "127.0.0.1:" + str(PORTS[service])


def prepare_credentials(run):
    secret = secrets.token_hex(32)
    unsigned = f"v1.9000.{int(time.time()) + 2*86400}.ops"
    token = unsigned + "." + hmac.new(secret.encode(), unsigned.encode(), hashlib.sha256).hexdigest()
    private_write(run / "scrape-token", token)
    member=f"v1.9001.{int(time.time())+2*86400}.user"
    private_write(run/'member-token',member+'.'+hmac.new(secret.encode(),member.encode(),hashlib.sha256).hexdigest())
    private_write(run / "token.key", secrets.token_hex(32))
    producer_file = run / "producers.json"
    # Operators can provision producers in this private file. Keep their credentials on resume.
    current = {}
    if producer_file.exists():
        from .observer.server import read_credentials
        read_credentials(producer_file)
        current = json.loads(producer_file.read_text(encoding="utf-8"))
    defaults = {
        **{name + "-sim": {"domain": name, "simulated": True, "token": secrets.token_urlsafe(32)}
           for name in ("dag", "agent", "device")},
        "experiment-lab": {"domain": "experiment", "simulated": False, "token": secrets.token_urlsafe(32)},
        "nebula-probe": {"domain": "service", "simulated": False, "token": secrets.token_urlsafe(32)},
        "nebula-functions": {"domain": "function", "simulated": False, "token": secrets.token_urlsafe(32)},
    }
    for name, row in defaults.items():
        if name not in current:
            current[name] = row
    require(len(current)<=16,"At most 16 telemetry producers are allowed")
    private_write(producer_file, json.dumps(current))
    return secret


def check_ports(ports):
    with ExitStack() as sockets:
        for name in ports:
            sock = sockets.enter_context(socket.socket())
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                sock.bind(("127.0.0.1", PORTS[name]))
                sock.listen(1)
            except OSError as error:
                raise RuntimeError("Monitoring port already in use: " + local(name)) from error


def start_process(stack, run, name, command):
    log = stack.enter_context((run / (name + ".log")).open("wb"))
    child = subprocess.Popen(list(map(str, command)), cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                             env={**clean_environment(),**({'GOMEMLIMIT':'256MiB','GOMAXPROCS':'2'} if name=='prometheus' else {})})
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


def await_url(url, process=None, name=None):
    until = time.monotonic() + 35
    while time.monotonic() < until:
        if process is not None and process.poll() is not None:
            raise RuntimeError(f"{name} exited with status {process.returncode}; see {name}.log")
        try:
            with urllib.request.urlopen(url, timeout=2) as r:
                if r.status == 200:
                    return
        except OSError:
            time.sleep(.3)
    raise RuntimeError("No ready response: " + url)


def disk_used(path):
    return sum(item.stat().st_size for item in path.rglob("*") if item.is_file() and not item.is_symlink())


def health_sample(run, port, server, token, account, otlp):
    from .probe import probe_http, trace_request
    from .scenarios import post
    status = {"targetPort": port}
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
        try:
            code, body = user.request("GET", "/api/conversations")
            if code == 401:
                login, data = user.request("POST", "/api/auth/login", {"username": username, "password": password})
                if login == 200:
                    user.csrf = data["csrfToken"]
                    code, body = user.request("GET", "/api/conversations")
            status["syntheticHttp"] = code
            status["syntheticShapeOk"] = code == 200 and isinstance(body, list)
        except (OSError, http.client.HTTPException, ValueError):
            status["syntheticHttp"] = 0
            status["syntheticShapeOk"] = False
        try:
            started = time.monotonic()
            status["traceId"], status["traceProbeHttp"] = trace_request(port, otlp, user.cookie)
            code = status["traceProbeHttp"]
            event = {"schemaVersion":1, "eventId":"probe-"+status["traceId"], "eventType":"service.event",
                     "occurredAt":str(time.time_ns()), "traceId":status["traceId"],
                     "fields":{"operation":"conversations.probe", "state":"success" if code==200 else "failed",
                               "durationMs":round((time.monotonic()-started)*1000,3)}}
            if code != 200:
                event["fields"]["errorCode"] = "http-"+str(code) if code else "connection"
            source = json.loads((run/"producers.json").read_text())["nebula-probe"]["token"]
            post("http://"+local("observer"), "/ingest/v1/events", [event], source)
        except (RuntimeError, OSError):
            status["traceProbe"] = "export-failed"
    status["timestamp"] = int(time.time())
    with (run / "soak.ndjson").open("a", encoding="utf-8") as log:
        log.write(json.dumps(status) + "\n")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--work-dir", type=Path, required=True)
    p.add_argument("--dependencies", type=Path)
    p.add_argument("--profile", choices=("full", "local", "metrics"), default="full")
    p.add_argument("--postgres-prefix", type=Path, required=True)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--duration", type=int, default=3600, metavar="SECONDS")
    p.add_argument("--data-budget-mib", type=int, help="Lower the profile's maximum experimental data usage")
    p.add_argument("--exercise", action="store_true", help="Read conversations once per minute as an isolated synthetic account")
    p.add_argument("--http-trace-every", type=int, default=20, metavar="N",
                   help="Sample the first and then every Nth unparented HTTP request per worker; 0 keeps failures only")
    p.add_argument("--ws-trace-every", type=int, default=0, metavar="N",
                   help="Sample the first and then every Nth chat.send per worker; 0 disables")
    p.add_argument("--resume-data", action="store_true", help="Reuse an existing lab's telemetry; business database is still disposable")
    p.add_argument("--source-network", action="append", default=[], help="Allowed non-loopback monitoring target CIDR")
    p.add_argument("--observer-port",type=int,default=PORTS["observer"])
    p.add_argument('--prometheus-port',type=int,default=PORTS['prometheus'])
    p.add_argument('--node-port',type=int,default=PORTS['node'])
    p.add_argument('--probe-port',type=int,default=PORTS['probe'])
    p.add_argument("--dual-node", action="store_true", help="Run two application processes against the owned database")
    p.add_argument("--replica-probe-port", type=int, default=18784)
    p.add_argument("--allow-primary-crash", action="store_true", help="Keep node B and monitoring up after an owned node A SIGKILL")
    p.add_argument("--with-alertmanager", action="store_true", help="Use the local Alertmanager and test receiver")
    p.add_argument("--alertmanager-port", type=int, default=19093)
    p.add_argument("--alert-receiver-port", type=int, default=19094)
    p.add_argument("--source-auth-file",type=Path,help="Private data source authentication configuration")
    p.add_argument("--resource-target",action="append",default=[],metavar="SERVICE=PID",
                   help="Additional local Linux process to sample")
    p.add_argument("--dashboards-dir",type=Path,help="Apply local dashboard files on startup")
    p.add_argument("--gateway-envoy", type=Path, help="Existing Envoy executable for the isolated metrics lab")
    p.add_argument("--gateway-port", type=int, default=18095)
    p.add_argument("--gateway-admin-port", type=int, default=19097)
    p.add_argument("--gateway-https-port", type=int)
    p.add_argument("--gateway-cert", type=Path)
    p.add_argument("--gateway-key", type=Path)
    p.add_argument("--gateway-wasm", type=Path, help="Optional prebuilt stream telemetry module")
    p.add_argument("--gateway-wasm-sha256", help="SHA-256 of the exact local Wasm module")
    args = p.parse_args()
    require(sys.platform.startswith("linux") and os.geteuid() > 0, "Use an unprivileged Linux account")
    network_isolation = require_isolated_network() if args.dual_node else None
    require(not args.dual_node or not args.source_network, "Dual-instance targets must stay in the private namespace")
    require(1 <= args.duration <= 86400, "Duration must be 1–86400 seconds")
    require(0 <= args.http_trace_every <= 1000000, "HTTP trace sample interval must be 0–1000000")
    require(0 <= args.ws_trace_every <= 1000000, "WS trace sample interval must be 0–1000000")
    full = args.profile == "full"
    metrics=full or args.profile=='metrics'
    profile_budget_mib = 512 if args.profile == "metrics" else 1024
    budget_mib = profile_budget_mib if args.data_budget_mib is None else args.data_budget_mib
    require(0 < budget_mib <= profile_budget_mib, "Data budget must be positive and within the chosen profile")
    data_budget = budget_mib * MIB
    require(1024 <= args.observer_port <= 65535 and args.observer_port not in
            [p for name,p in PORTS.items() if name!="observer"], "Invalid Observer port")
    PORTS["observer"]=args.observer_port
    for name in ('prometheus','node','probe'):
        PORTS[name]=getattr(args,name+'_port')
    require(not args.allow_primary_crash or args.dual_node, "Crash exercises require --dual-node")
    require(not args.with_alertmanager or metrics, "Alertmanager requires the metrics or full profile")
    additions = {}
    if metrics and args.dual_node:
        additions["probe_b"] = args.replica_probe_port
    if args.with_alertmanager:
        additions.update(alertmanager=args.alertmanager_port, alert_receiver=args.alert_receiver_port)
    gateway.check_ports(list(additions.values()))
    PORTS.update(additions)
    require(all(1024<=value<=65535 for value in PORTS.values()) and len(set(PORTS.values()))==len(PORTS),'Monitoring ports must be distinct unprivileged ports')
    require(not metrics or args.dependencies is not None, "The metrics/full profile requires --dependencies")
    binaries = inputs(args) if metrics else {}
    gateway_binary = gateway.options(args)
    if gateway_binary:
        require(metrics, "The optional gateway requires the metrics or full profile")
        PORTS["gateway"] = args.gateway_port
        PORTS["gateway_admin"] = args.gateway_admin_port
        if args.gateway_https_port:
            PORTS["gateway_https"] = args.gateway_https_port
        require(len(set(PORTS.values())) == len(PORTS), "Gateway and monitoring ports must be distinct")
    names = list(PORTS) if full else ["observer", "prometheus", "node", "probe"] if metrics else ["observer"]
    if not full:
        names += list(additions)
    if gateway_binary and not full:
        names += [name for name in ("gateway", "gateway_admin", "gateway_https") if name in PORTS]
    check_ports(names)
    run = args.work_dir.resolve()
    if args.resume_data:
        require((run / "session.json").is_file() and (run / "events.sqlite").is_file(), "Use a former lab directory")
    else:
        run.mkdir(mode=0o700, parents=True, exist_ok=False)
    os.umask(0o077)
    from .retention import mark,prune
    prune(run.parent,apply=True)
    if not args.resume_data:mark(run)
    require(shutil.disk_usage(run).free >= DISK_RESERVE + data_budget,
            f"Monitoring lab requires {1024 + budget_mib} MiB free for its data budget and disk reserve")
    done = threading.Event()
    for name in (signal.SIGINT, signal.SIGTERM):
        signal.signal(name, lambda *_: done.set())
    # The normal NEBULA runner allocates a disposable TCP port and owns the child.
    with disposable_database(args.postgres_prefix.resolve(), ROOT, run) as (connection, migration):
        env = {**clean_environment(), "WEB_PHASE11_REQUIRED": "1", "WEB_FUNCTION3_ENABLE": "1",
               "WEB_GRPC_ADDRESS":"127.0.0.1:0",
               "WEB_PHASE11_DATABASE": connection, "WEB_PHASE11_TOKEN_KEY_FILE": str(run / "token.key"),
               "WEB_PHASE11_FILE_ROOT": str(run / "files"), "WEB_FUNCTION3_MEDIA_ROOT": str(run / "media"),
               "WEB_OBSERVABILITY": "1", "WEB_OBSERVABILITY_LOG": "error",
               "WEB_BUSINESS_LOG_LEVEL": "warn",
               "WEB_BUSINESS_LOG_DIRECTORY":str(run/"structured-logs"),
               "WEB_OBSERVABILITY_ENVIRONMENT": "lab",
               "WEB_OBSERVABILITY_HTTP_TRACE_EVERY": str(args.http_trace_every),
               "WEB_OBSERVABILITY_WS_TRACE_EVERY": str(args.ws_trace_every)}
        otlp = "http://" + local("collector" if full else "observer")
        env["WEB_OBSERVABILITY_OTLP"] = otlp
        if gateway_binary:
            env["WEB_OBSERVABILITY_GATEWAY_TRACE_HEADER"] = "1"
            if args.gateway_https_port:
                env["WEB_PHASE11_SECURE_COOKIE"] = "1"
            # The two local aliases are explicit browser Origins, including the port.
            env["WEB_ALLOWED_ORIGINS"] = ",".join(
                [f"http://{host}:{args.gateway_port}" for host in ("127.0.0.1", "localhost")]
                + ([f"https://{host}:{args.gateway_https_port}" for host in ("127.0.0.1", "localhost")]
                   if args.gateway_https_port else []) + (["http://localhost"] if args.dual_node else []))
        # Prepare the server key before the ephemeral target port is known.
        env["WEB_AUTH_SECRET"] = prepare_credentials(run)
        private_write(run / "lab.pid", str(os.getpid()))
        with ExitStack() as stack:
            binary = args.binary.resolve()
            primary_env = {**env, "WEB_PHASE11_REALTIME_RELAY": "1", "WEB_OBSERVABILITY_INSTANCE_ID": "nebula-a"} if args.dual_node else env
            target, server = stack.enter_context(owned_server_process(binary, ROOT, primary_env, run/"nebula.log",
                expected_exit=(0, -signal.SIGKILL) if args.allow_primary_crash else 0))
            primary = "nebula-a" if args.dual_node else f"127.0.0.1:{target}"
            targets = [(primary, target, "probe")]
            nodes = {primary: (target, server)}
            if args.dual_node:
                replica_env = {**primary_env, "WEB_OBSERVABILITY_INSTANCE_ID": "nebula-b",
                               "WEB_BUSINESS_LOG_DIRECTORY": str(run/"structured-logs-b")}
                replica_target, replica = stack.enter_context(owned_server_process(binary, ROOT, replica_env, run/"nebula-b.log"))
                targets.append(("nebula-b", replica_target, "probe_b"))
                nodes["nebula-b"] = (replica_target, replica)
            observer = [sys.executable, "-m", "tools.observability.observer.server", "serve",
                        "--db", run / "events.sqlite", "--credentials", run / "producers.json",
                        "--port", PORTS["observer"]]
            observer += ["--log-input-directory", (primary if args.dual_node else "nebula-webserver")+"="+str(run/"structured-logs")]
            if args.dual_node:
                observer += ["--log-input-directory", "nebula-b="+str(run/"structured-logs-b"),
                             "--resource-target", "NEBULA-B="+str(replica.pid)]
            postgres_pid=Path(connection.split()[0].removeprefix("host="))/"data/postmaster.pid"
            if postgres_pid.is_file():
                observer += ["--resource-target","PostgreSQL="+postgres_pid.read_text().splitlines()[0]]
            observer += ["--resource-target", ("NEBULA-A" if args.dual_node else "NEBULA")+"="+str(server.pid)]
            for target_spec in args.resource_target:
                observer += ["--resource-target",target_spec]
            dashboards = args.dashboards_dir
            if gateway_binary and dashboards is None:
                dashboards = ROOT / "config/observability/dashboards"
            if dashboards:
                observer += ["--dashboards-dir", dashboards.resolve()]
            gateway_process = None
            if gateway_binary:
                gateway_dir = run / "gateway"
                gateway_process = stack.enter_context(gateway.start(
                    gateway_binary, gateway_dir, args, target, replica_port=replica_target if args.dual_node else None))
                await_url("http://" + local("gateway_admin") + "/ready")
                require(gateway_process.poll() is None, "Envoy exited during startup; see gateway/envoy-stderr.log")
                observer += ["--gateway-access-log", gateway_dir / "access.jsonl",
                             "--resource-target", "Ingress=" + str(gateway_process.pid)]
            for network in args.source_network:
                observer += ["--source-network",network]
            if args.source_auth_file:
                observer += ["--source-auth-file",args.source_auth_file.resolve()]
            if metrics:
                lab_config.write(run, PORTS, targets, private_write, full=full)
                subprocess.run([str(binaries["promtool"]), "check", "config", str(run / "prometheus.yaml")],
                               timeout=10, check=True, env=clean_environment())
                if args.with_alertmanager:
                    subprocess.run([str(binaries["amtool"]), "check-config", str(run/"alertmanager.yaml")],
                                   timeout=10, check=True, env=clean_environment())
            if not full:
                observer += ["--local-target", target, "--local-target-pid", server.pid,
                             "--scrape-token-file", run / "scrape-token", "--local-otlp",'--require-ops',
                             '--member-token-file',run/'member-token']
                if metrics:
                    observer += ['--metrics-prometheus','http://'+local('prometheus'),'--probe','http://'+local('probe')]
            commands = {"observer": observer}
            if metrics:
                commands.update({
                    "node": [binaries["node"], "--web.listen-address=" + local("node")],
                    "prometheus": [binaries["prometheus"], "--config.file=" + str(run / "prometheus.yaml"),
                                   "--web.listen-address=" + local("prometheus"),
                                   "--storage.tsdb.path=" + str(run / "prometheus"),
                                   "--storage.tsdb.retention.time=7d",
                                   f"--storage.tsdb.retention.size={min(384, budget_mib)}MB"],
                })
                for instance, node_port, probe_name in targets:
                    commands[probe_name] = [sys.executable, "-m", "tools.observability.probe", "serve", "--target-port", node_port,
                                            "--target-pid", nodes[instance][1].pid, "--port", PORTS[probe_name]]
                if args.with_alertmanager:
                    commands.update({"alert_receiver": [sys.executable, "-m", "tools.observability.alert_receiver",
                                                         "--port", PORTS["alert_receiver"]],
                        "alertmanager": [binaries["alertmanager"], "--config.file="+str(run/"alertmanager.yaml"),
                                         "--web.listen-address="+local("alertmanager"), "--cluster.listen-address=",
                                         "--storage.path="+str(run/"alertmanager"), "--data.retention=168h"]})
            if full:
                commands.update({'tempo':[binaries['tempo'],'-config.file='+str(run/'tempo.yaml')],
                    'collector':[binaries['collector'],'--config='+str(run/'collector.yaml')]})
            # Publish the already-ready target before Observer can report /health.
            # A resumed lab must never advertise the former process's TCP port.
            ports = {name: PORTS[name] for name in names}
            private_write(run / "session.json", json.dumps({"nebulaPort": target, "migration": migration,
                "profile": args.profile, "ports": ports, "httpTraceEvery": args.http_trace_every,
                "wsTraceEvery": args.ws_trace_every, "gatewayWasm": bool(args.gateway_wasm),
                "allowPrimaryCrash": args.allow_primary_crash, "networkIsolation": network_isolation,
                "instances": {name: {"port": port, "pid": child.pid} for name, (port, child) in nodes.items()}}))
            processes = {name: start_process(stack, run, name, cmd) for name, cmd in commands.items()}
            await_url("http://" + local("observer") + "/health", processes["observer"], "observer")
            if metrics:
                await_url("http://" + local("prometheus") + "/-/ready", processes["prometheus"], "prometheus")
            if full:
                await_url("http://" + local("tempo") + "/ready", processes["tempo"], "tempo")
                await_url("http://" + local("collector_metrics") + "/metrics", processes["collector"], "collector")
            if args.with_alertmanager:
                await_url("http://" + local("alert_receiver") + "/health", processes["alert_receiver"], "alert_receiver")
                await_url("http://" + local("alertmanager") + "/-/ready", processes["alertmanager"], "alertmanager")
            account = None
            if args.exercise:
                client_options = {}
                client_port = target
                if gateway_binary:
                    client_port = args.gateway_https_port or args.gateway_port
                    scheme = "https" if args.gateway_https_port else "http"
                    client_options["origin"] = f"{scheme}://127.0.0.1:{client_port}"
                    if args.gateway_https_port:
                        client_options["tls_context"] = ssl.create_default_context(cafile=str(args.gateway_cert))
                user = browser_fixture(ROOT).BrowserClient("127.0.0.1", client_port, **client_options)
                username, password = "obs_soak_" + secrets.token_hex(6), secrets.token_urlsafe(20)
                user.register_and_login(username, password)
                account = user, username, password
            if args.dual_node:
                private_write(run / "ready.pid", str(os.getpid()))
            print(json.dumps({"observer": "http://" + local("observer"), "target": target}), flush=True)
            start = time.monotonic()
            last_sample = start - 60
            scrape_token = (run / "scrape-token").read_text()
            while not done.wait(2):
                dead = [name for name, child in processes.items() if child.poll() is not None]
                require(not dead, "Monitoring component exited: " + ", ".join(dead))
                primary_alive = server.poll() is None
                require(primary_alive or (args.allow_primary_crash and server.returncode == -signal.SIGKILL),
                        "Primary NEBULA process exited unexpectedly")
                if args.dual_node:
                    require(replica.poll() is None, "Replica NEBULA process exited unexpectedly")
                if gateway_process:
                    require(gateway_process.poll() is None, "Envoy exited; see gateway/envoy-stderr.log")
                    require(disk_used(gateway_dir) < gateway.DATA_BUDGET,
                            "Gateway reached its disk budget; stopping the owned lab")
                if time.monotonic() - start >= args.duration:
                    break
                require(disk_used(run) < data_budget and shutil.disk_usage(run).free > DISK_RESERVE,
                        "Monitoring lab reached its disk budget; stopping gracefully")
                if full:
                    require(disk_used(run/"tempo") < 256*1024**2, "Tempo reached its trace data budget")
                if time.monotonic() - last_sample >= 60:
                    if primary_alive:
                        health_sample(run, target, server, scrape_token, account, otlp)
                    else:
                        health_sample(run, replica_target, replica, scrape_token, None, otlp)
                    last_sample = time.monotonic()


if __name__ == "__main__":
    main()
