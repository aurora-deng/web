"""Real PostgreSQL lease takeover using two native worker processes and one Observer trace."""

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import secrets
import selectors
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / "scripts/project")]
from repro_runtime import disposable_database, get_json
from tools.observability.observer.server import ObserverServer
from tools.observability.observer.sources import Sources
from tools.observability.observer.store import Store


def process_ready(process, output):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        assert selector.select(6), "worker did not become ready"
        assert process.stdout.readline().strip() == output


@contextmanager
def service(binary, env, owner, lease):
    process = subprocess.Popen([binary, "serve", owner, str(lease)], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        process_ready(process, f"READY {owner}")
        yield
    finally:
        if process.poll() is None:
            process.terminate()
        output, error = process.communicate(timeout=8)
        assert process.returncode == 0, (output, error)


def wait_state(psql, run, expected):
    for _ in range(160):
        state = query(psql, f"SELECT to_json(workflow_cluster_run_state('{run}'))")
        if state == expected:
            return
        time.sleep(0.03)
    raise AssertionError(f"{run}: expected {expected}, got {state}")


def takeover(binary, env, psql, run, effect):
    command = [str(binary)]
    trace = subprocess.check_output([*command, "submit", run, effect], env=env, text=True).strip()
    assert subprocess.check_output([*command, "submit", run, effect], env=env, text=True).strip() == trace
    a = subprocess.Popen([*command, "work", run, effect, "worker-a", "500", "hold"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    paused = False
    try:
        process_ready(a, "CLAIM A 1 " + trace)
        a.send_signal(signal.SIGSTOP)
        paused = True
        with service(binary, env, "worker-b", 1500):
            wait_state(psql, run, "uncertain" if effect == "external" else "success")
        a.send_signal(signal.SIGCONT)
        paused = False
        late, error = a.communicate("finish\n", timeout=5)
        assert a.returncode == 3 and late.strip() == "STALE_LEASE", (late, error)
        return trace
    finally:
        if paused:
            a.send_signal(signal.SIGCONT)
        if a.poll() is None:
            a.kill()
            a.wait(timeout=5)


def query(psql, sql):
    output = subprocess.check_output([*psql, "-qAt", "-c", sql], text=True)
    return json.loads(output)


@contextmanager
def observer(root):
    token = secrets.token_hex(24)
    sources = Sources("http://127.0.0.1:1", "http://127.0.0.1:1")
    server = ObserverServer(("127.0.0.1", 0), Store(root / "observer.sqlite"), sources,
                            [(token, "workflow-pg", "dag", True)])
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        yield server.server_port, token
    finally:
        server.shutdown()
        thread.join()
        server.server_close()


def export_events(psql, connection, port, token, *, check=True):
    params = dict(item.split("=", 1) for item in shlex.split(connection))
    return subprocess.run([sys.executable, "-B", ROOT / "scripts/project/workflow_observer.py",
        "--psql", psql[0], "--observer", f"http://127.0.0.1:{port}"],
        env=dict(os.environ, PGHOST=params["host"], PGPORT=params["port"], PGUSER=params["user"],
                 PGDATABASE=params["dbname"], NEBULA_FLEET_OBSERVER_TOKEN=token),
        timeout=18, text=True, capture_output=True, check=check)


def verify_external(binary, psql, env):
    command = [binary, "reconcile", "external-test", "A", "1", "success", "verified-receipt", "checked"]
    for _ in range(2):
        assert subprocess.check_output(command, env=env, text=True, timeout=8).strip() == "VERIFIED"
    conflict = subprocess.run([*command[:-1], "conflict"], env=env, capture_output=True, text=True, timeout=8)
    assert conflict.returncode == 3 and conflict.stdout.strip() == "VERIFICATION_CONFLICT"
    with service(binary, env, "worker-after-verification", 500):
        wait_state(psql, "external-test", "success")
    assert query(psql, "SELECT to_json(count(*)) FROM workflow_cluster_verification "
                       "WHERE run_id='external-test' AND evidence_ref='verified-receipt'") == 1
    assert query(psql, "SELECT to_json(count(*)) FROM workflow_cluster_attempt "
                       "WHERE run_id='external-test' AND node_id='A'") == 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--postgres", type=Path, required=True)
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nebula-workflow-pg-") as directory:
        root = Path(directory)
        with disposable_database(args.postgres, ROOT, root) as (connection, version):
            psql = [args.postgres / "bin/psql", connection, "-X", "-v", "ON_ERROR_STOP=1"]
            for filename in ("schema.sql", "upgrade_v2.sql"):
                migrated = subprocess.run([*psql, "-f", ROOT / "backend/systems/PostgresWorkflowQueue" / filename],
                                          capture_output=True, text=True)
                assert migrated.returncode == 0, migrated.stderr
            env = dict(os.environ, NEBULA_WORKFLOW_DSN=connection, NEBULA_WORKFLOW_RECONCILE_DSN=connection)
            trace = takeover(args.worker, env, psql, "lease-test", "read")
            unknown = takeover(args.worker, env, psql, "external-test", "external")
            attempts = query(psql, """
                SELECT json_agg(json_build_array(node_id,attempt,owner,
                      (extract(epoch FROM started_at)*1000000000)::bigint,
                      (extract(epoch FROM ended_at)*1000000000)::bigint,state) ORDER BY node_id,attempt)
                FROM workflow_cluster_attempt WHERE run_id='lease-test'
            """)
            assert [(r[0],r[1],r[2],r[5]) for r in attempts] == [
                ("A",1,"worker-a","timeout"),("A",2,"worker-b","success"),("B",1,"worker-b","success"),
                ("C",1,"worker-b","success"),("D",1,"worker-b","success")]
            assert query(psql, """
                SELECT json_agg(json_build_array(node_id,state,attempt) ORDER BY node_id)
                FROM workflow_cluster_node WHERE run_id='external-test'
            """) == [["A","uncertain",1],["B","skipped",0],["C","skipped",0],["D","skipped",0]]
            subprocess.check_output([args.worker, "submit", "renew-test", "slow"], env=env, timeout=8)
            with service(args.worker, env, "renew-a", 240), service(args.worker, env, "renew-b", 240):
                wait_state(psql, "renew-test", "success")
            renewed = query(psql, "SELECT json_agg(json_build_array(attempt,state,"
                "(extract(epoch FROM ended_at-started_at)*1000)::int)) FROM workflow_cluster_attempt "
                "WHERE run_id='renew-test' AND node_id='A'")
            assert len(renewed) == 1 and renewed[0][:2] == [1, "success"] and renewed[0][2] > 600

            with observer(root) as (port, token):
                pending = "SELECT to_json(count(*)) FROM workflow_cluster_event WHERE exported_at IS NULL"
                assert export_events(psql, connection, 1, token, check=False).returncode != 0
                assert query(psql, pending) > 0
                export_events(psql, connection, port, token)
                before = get_json(port, "/api/v1/traces/" + unknown)["items"]
                assert any(row["eventType"] == "dag.run" and row["fields"]["state"] == "uncertain" for row in before)
                verify_external(args.worker, psql, env)
                export_events(psql, connection, port, token)
                after = get_json(port, "/api/v1/traces/" + unknown)["items"]
                assert any(row["fields"].get("errorCode") == "verified_outcome" for row in after)
                assert any(row["eventType"] == "dag.run" and row["fields"]["state"] == "success" for row in after)
                telemetry = len(get_json(port, "/api/v1/traces/" + trace)["items"])
                assert telemetry >= 16 and query(psql, pending) == 0
                query(psql, "UPDATE workflow_cluster_event SET exported_at=NULL WHERE id=1; SELECT 'true' AS replay")
                export_events(psql, connection, port, token)
                assert len(get_json(port, "/api/v1/traces/" + trace)["items"]) == telemetry
    args.report.write_text(json.dumps({"passed": True, "businessMigration": version,
        "idempotentTraceId": trace, "externalTraceId": unknown, "attempts": attempts,
        "rejectedLateWrites": 2, "workerDiscovery": True, "renewedAttemptMs": renewed[0][2],
        "verifiedExternalOnce": True, "observerHttp": True, "observerEvents": telemetry,
        "observerDeliveryRetry": True}, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
