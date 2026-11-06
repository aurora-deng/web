"""Run only resources owned by a reproduction session (no existing DB or service)."""
from contextlib import contextmanager, closing
import http.client
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time

DATABASE = "nebula_repro_test"


def require(condition, reason):
    if not condition:
        raise RuntimeError(reason)


def require_tool(path):
    require(path.is_file() and os.access(path, os.X_OK), f"Missing installed executable: {path}")
    return str(path)


def clean_environment():
    # Do not inherit a developer's real business DB, model keys or database client settings.
    return {key: value for key, value in os.environ.items()
            if not key.startswith(("WEB_", "PG", "PHASE11_", "FUNCTION3_"))}


@contextmanager
def disposable_database(prefix, source, build):
    tools = {name: require_tool(prefix / "bin" / name)
             for name in ("initdb", "pg_ctl", "psql", "createdb")}
    require(os.geteuid() != 0, "Business reproduction must run as an unprivileged user")
    # Keep the Unix socket path short even with a deeply nested build directory.
    temporary = Path(tempfile.mkdtemp(prefix="nebula-pg-", dir="/tmp"))
    data = temporary / "data"
    env = clean_environment()
    port, user = "5432", "nebula_repro"
    flags = ["-h", str(temporary), "-p", port, "-U", user]
    started = False
    try:
        subprocess.run([tools["initdb"], "-D", str(data), "-U", user,
                        "--auth-local=trust", "--auth-host=reject", "--encoding=UTF8",
                        "--locale=C"], check=True, env=env, stdout=subprocess.DEVNULL)
        options = f"-k {shlex.quote(str(temporary))} -p {port} -c listen_addresses='' -c max_connections=40"
        subprocess.run([tools["pg_ctl"], "-D", str(data), "-l", str(build / "postgres.log"),
                        "-o", options, "-w", "-t", "30", "start"], check=True, env=env)
        started = True
        subprocess.run([tools["createdb"], *flags, "-w", DATABASE], check=True, env=env)
        directory = source / "backend/application/storage/postgres/migrations"
        migrations = sorted(directory.glob("[0-9][0-9][0-9]_*.sql"))
        versions = [int(path.name[:3]) for path in migrations]
        require(versions and versions == list(range(1, versions[-1] + 1)),
                "Missing or repeated numbered migration")
        psql = [tools["psql"], "-X", "-w", *flags, "-d", DATABASE, "-v", "ON_ERROR_STOP=1"]
        with (build / "migrations.log").open("wb") as log:
            for path in migrations:
                subprocess.run([*psql, "-f", str(path)], check=True, env=env, stdout=log, stderr=log)
        seen = subprocess.check_output([*psql, "-qAtc",
            "SELECT version FROM phase11_schema_migrations ORDER BY version"], env=env, text=True)
        require(seen.split() == list(map(str, versions)), "Applied migration ledger differs from source")
        connection = f"host={temporary} port={port} user={user} dbname={DATABASE} connect_timeout=5"
        yield connection, versions[-1]
    finally:
        # If start timed out after spawning Postgres, still try to stop our own instance.
        if started or (data / "postmaster.pid").is_file():
            subprocess.run([tools["pg_ctl"], "-D", str(data), "-m", "fast",
                            "-w", "-t", "30", "stop"], check=True, env=env)
        shutil.rmtree(temporary)


def get(port, route):
    with closing(http.client.HTTPConnection("127.0.0.1", port, timeout=2)) as connection:
        connection.request("GET", route)
        response = connection.getresponse()
        return response.status, response.read()


def get_json(port, route):
    status, body = get(port, route)
    require(status == 200, f"{route}: expected 200, got {status}")
    return json.loads(body)


@contextmanager
def owned_server_process(binary, root, env, log_path, expected_exit=0):
    # The frozen server binds all interfaces. Run in a trusted VM or network namespace.
    with closing(socket.socket()) as reserved:
        reserved.bind(("", 0))
        port = reserved.getsockname()[1]
    env = {**env, "WEB_SERVER_PORT": str(port), "WEB_SERVER_REACTORS": "2",
           "WEB_HTTP_WORKERS": "2", "WEB_WS_WORKERS": "2"}
    with log_path.open("wb") as log:
        child = subprocess.Popen([str(binary)], cwd=root, env=env,
                                 stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                require(child.poll() is None, f"Server exited; see {log_path.name}")
                try:
                    if get_json(port, "/health/ready") == {"status": "ready"}:
                        break
                except (OSError, http.client.HTTPException, RuntimeError):
                    pass
                time.sleep(0.1)
            else:
                raise RuntimeError(f"Server not ready; see {log_path.name}")
            yield port, child
        finally:
            already_failed = sys.exc_info()[0] is not None
            if child.poll() is None:
                child.terminate()
            try:
                status = child.wait(timeout=20)
            except subprocess.TimeoutExpired:
                child.kill()
                status = child.wait()
            if not already_failed:
                require(status == expected_exit, f"Server exited with {status}; see {log_path.name}")


@contextmanager
def owned_server(binary, root, env, log_path):
    with owned_server_process(binary, root, env, log_path) as (port, _):
        yield port


def http_smoke(root, port, expect_business):
    require(get_json(port, "/health/live") == {"status": "alive"}, "Liveness mismatch")
    stamp = get_json(port, "/health/version")
    require(stamp["schemaVerifiedAtStartup"] is expect_business, "Wrong runtime business mode")
    require(get(port, "/fast")[0] == 200, "/fast is unavailable")
    require(get(port, "/phase11/") == (200, (root / "frontend/phase11/index.html").read_bytes()),
            "Login page differs from this checkout")
    manifest = get_json(port, "/phase11/asset-manifest.json")
    require(manifest == json.loads((root / "frontend/phase11/asset-manifest.json").read_text()),
            "Frontend manifest differs from this checkout")
    expected = 401 if expect_business else 503
    require(get(port, "/api/me")[0] == expected, f"/api/me should return {expected} without a session")
    return {"revision": stamp["revision"], "sourceFingerprint": stamp["sourceFingerprint"],
            "assetVersion": manifest["version"], "schemaVerifiedAtStartup": expect_business,
            "requiredMigration": stamp["requiredMigration"]}


def browser_fixture(root):
    spec = importlib.util.spec_from_file_location("nebula_repro_browser",
        root / "tests/phase11/integration/runtime_blackbox.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    module.TIMEOUT = 6.0
    return module


def business_smoke(port, browser):
    client = browser.BrowserClient("127.0.0.1", port)
    name = "repro_" + secrets.token_hex(8)
    client.register_and_login(name, secrets.token_urlsafe(20))
    status, categories = client.request("GET", "/api/tree/categories")
    require(status == 200 and bool(categories), "No story categories after migration")
    status, speech = client.request("GET", "/api/me/speech/status")
    require(status == 200 and speech["delivery"] == "local" and not speech["enabled"],
            "Unexpected event delivery or speech provider")
    ws, _ = client.open_websocket()
    ws.close()
    sse, reader = client.open_sse()
    with closing(sse):
        request = {"requestId": name, "categoryId": categories[0]["id"], "body": "Reproduction check"}
        status, post = client.request("POST", "/api/tree/posts", request)
        require(status == 201, "Story creation failed")
        status, repeated = client.request("POST", "/api/tree/posts", request)
        require(status == 201 and repeated["id"] == post["id"], "Story was not idempotent")
        for _ in range(50):
            event = reader.read()
            if "event: tree.post.created\n" not in event:
                continue
            body = browser.sse_json(event)
            payload = body["payload"]
            if isinstance(payload, str):
                payload = json.loads(payload)
            if str(payload.get("postId")) == str(post["id"]):
                return client, str(post["id"])
        raise RuntimeError("No matching SSE event from the durable publisher")
