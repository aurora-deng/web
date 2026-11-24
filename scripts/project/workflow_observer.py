"""Deliver committed PostgreSQL workflow telemetry with explicit outbox acknowledgements."""

import argparse
import http.client
import json
import os
from pathlib import Path
import subprocess
import time

from fleet_observer import local_observer_port, post_events, producer_token


def query(psql, statement):
    env = dict(os.environ, PGCONNECT_TIMEOUT="5", PGOPTIONS="-c statement_timeout=5000 -c lock_timeout=2000")
    result = subprocess.run([psql, "-XqAtw", "-v", "ON_ERROR_STOP=1"], input=statement, env=env,
                            text=True, capture_output=True, timeout=12)
    if result.returncode:
        raise RuntimeError("workflow Observer database operation failed")
    return result.stdout.strip()


def deliver(psql, port, token):
    batch = json.loads(query(psql, "SELECT coalesce(json_agg(e ORDER BY e.id),'[]'::json) "
                             "FROM workflow_cluster_export_batch() e;"))
    if not batch:
        return 0
    ids = [row["id"] for row in batch]
    if len(batch) > 64 or any(not isinstance(value, int) or value <= 0 for value in ids):
        raise ValueError("unexpected workflow event batch")
    post_events(port, [row["envelope"] for row in batch], token)
    query(psql, "UPDATE workflow_cluster_event SET exported_at=clock_timestamp() "
          "WHERE exported_at IS NULL AND id IN (" + ",".join(map(str, ids)) + ");")
    return len(batch)


def prune(psql):
    query(psql, """
        DELETE FROM workflow_cluster_event WHERE id IN (
            SELECT id FROM workflow_cluster_event
            WHERE occurred_at < clock_timestamp()-interval '7 days'
            ORDER BY occurred_at,id LIMIT 512 FOR UPDATE SKIP LOCKED
        );
    """)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--psql", type=Path, required=True)
    parser.add_argument("--observer", required=True)
    parser.add_argument("--follow", action="store_true", help="Wait for events and retry transient failures")
    args = parser.parse_args()
    if not args.psql.is_file() or not (os.environ.get("PGDATABASE") or os.environ.get("PGSERVICE")):
        parser.error("provide the installed psql and set PGDATABASE or PGSERVICE for the workflow database")
    try:
        port, token = local_observer_port(args.observer), producer_token()
    except ValueError as error:
        parser.error(str(error))
    next_prune = 0
    delay = 1
    sent = 0
    try:
        while True:
            try:
                now = time.monotonic()
                if now >= next_prune:
                    prune(args.psql)
                    next_prune = now + 60
                count = deliver(args.psql, port, token)
            except (OSError, RuntimeError, subprocess.TimeoutExpired, http.client.HTTPException) as error:
                if not args.follow:
                    parser.exit(1, f"{error}\n")
                print(f"Workflow Observer retry in {delay}s: {error}", flush=True)
                time.sleep(delay)
                delay = min(30, delay * 2)
                continue
            sent += count
            delay = 1
            if not count:
                if not args.follow:
                    break
                time.sleep(0.25)
    except KeyboardInterrupt:
        pass
    print(json.dumps({"eventsSent": sent}))


if __name__ == "__main__":
    main()
