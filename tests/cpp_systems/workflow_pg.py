"""Real PostgreSQL lease takeover using two native worker processes and one Observer trace."""

import argparse
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / "scripts/project")]
from repro_runtime import disposable_database
from tools.observability.observer.store import Store


def takeover(binary, env, run, effect):
    command = [str(binary)]
    trace = subprocess.check_output([*command, "submit", run, effect], env=env, text=True).strip()
    assert subprocess.check_output([*command, "submit", run, effect], env=env, text=True).strip() == trace
    a = subprocess.Popen([*command, "work", run, effect, "worker-a", "500", "hold"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    paused = False
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(a.stdout, selectors.EVENT_READ)
            assert selector.select(6), "first worker did not acquire a lease"
            assert a.stdout.readline().strip() == "CLAIM A 1 " + trace
        a.send_signal(signal.SIGSTOP)
        paused = True
        b = subprocess.run([*command, "work", run, effect, "worker-b", "1500"], env=env,
                            capture_output=True, text=True, timeout=12)
        if effect == "external":
            assert b.returncode == 2 and b.stdout.strip() == "RESULT uncertain", (b.stdout, b.stderr)
        else:
            assert b.returncode == 0 and "CLAIM A 2 " + trace in b.stdout and "RESULT success" in b.stdout, (b.stdout, b.stderr)
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


def trace_events(run, trace, attempt_rows, dependencies, started, final):
    def event(kind, fields, when, key, span=None):
        value = {"schemaVersion": 1, "eventId": trace + "." + key, "eventType": kind,
                 "occurredAt": str(when), "traceId": trace, "fields": {"runId": run, **fields}}
        if span:
            value["spanId"] = span
        return value

    yield event("dag.run", {"workflow": "fork-join.v1", "state": "started"}, started, "start")
    for before, after in dependencies:
        yield event("dag.edge", {"predecessor": before, "successor": after}, started, before + after)
    for index, (node, attempt, owner, begin, end, status) in enumerate(attempt_rows, 1):
        common = {"nodeId": node, "attempt": attempt, "workerId": owner}
        yield event("dag.node", {**common, "state": "started"}, begin, f"begin.{index}", f"{index:016x}")
        yield event("dag.node", {**common, "state": status, "durationMs": (end - begin) / 1e6},
                    end, f"end.{index}", f"{index:016x}")
    yield event("dag.run", {"workflow": "fork-join.v1", "state": final},
                max(row[4] for row in attempt_rows), "done")


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
            subprocess.run([*psql, "-f", ROOT / "backend/systems/PostgresWorkflowQueue/schema.sql"],
                           check=True, capture_output=True)
            env = dict(os.environ, NEBULA_WORKFLOW_DSN=connection)
            trace = takeover(args.worker, env, "lease-test", "read")
            unknown = takeover(args.worker, env, "external-test", "external")
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
            start = query(psql, "SELECT to_json((extract(epoch FROM created_at)*1000000000)::bigint) "
                                "FROM workflow_cluster_run WHERE run_id='lease-test'")
            events = list(trace_events("lease-test", trace, attempts,
                                       [("A","B"),("A","C"),("B","D"),("C","D")], start, "success"))
            observer = Store(root / "observer.sqlite")
            observer.append("pg-workflow-lab", "dag", True, events)
            replayed = observer.events(start - 1, time.time_ns(), 50, trace=trace)
            assert len(replayed["items"]) == len(events) and not replayed["next"]
            external = query(psql, "SELECT json_build_array((extract(epoch FROM ended_at)*1000000000)::bigint) "
                                   "FROM workflow_cluster_attempt WHERE run_id='external-test'")
            unknown_event = {"schemaVersion": 1, "eventId": unknown + ".end", "eventType": "dag.run",
                             "occurredAt": str(external[0]), "traceId": unknown,
                             "fields": {"runId": "external-test", "state": "uncertain"}}
            observer.append("pg-workflow-lab", "dag", True, [unknown_event])
            unknown_rows = observer.events(start - 1, time.time_ns(), 50, trace=unknown)
            assert unknown_rows["items"][0]["fields"]["state"] == "uncertain"
    args.report.write_text(json.dumps({"passed": True, "businessMigration": version,
        "idempotentTraceId": trace, "externalTraceId": unknown, "attempts": attempts,
        "rejectedLateWrites": 2, "observerEvents": len(events) + 1}, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
