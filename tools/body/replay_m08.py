"""Replay an Ubuntu M08 or guarded M09 run through an isolated Observer."""
import argparse
from contextlib import closing
import http.client
import json
import os
from pathlib import Path
import secrets
import threading

from scripts.project.fleet_observer import post_events
from tools.observability.observer.server import ObserverServer
from tools.observability.observer.sources import Sources
from tools.observability.observer.store import Store


def items(port, trace):
    with closing(http.client.HTTPConnection("127.0.0.1", port, timeout=4)) as connection:
        connection.request("GET", f"/api/v1/traces/{trace}")
        with connection.getresponse() as response:
            if response.status != 200:
                raise RuntimeError(f"Observer trace query returned {response.status}")
            return json.load(response)["items"]


def check(directory, database):
    guarded = directory / "m09-cross-vm.json"
    report = guarded if guarded.is_file() else directory / "m08-evidence.json"
    evidence = json.loads(report.read_text())
    phase = "m09-" if evidence.get("guardedActuator") else "m08-"
    if evidence.get("passed") is not True or database.exists():
        raise ValueError("choose passing lab evidence and a new Observer database")
    tasks = [*evidence["goals"], evidence["recovery"], evidence["disconnect"], evidence["cancellation"]]
    robots = evidence["robots"]
    if robots not in (["alpha"], ["alpha", "beta"]):
        raise ValueError("unknown lab topology")
    sources = {robot: secrets.token_hex(24) for robot in robots}
    batches = {}
    for robot in sources:
        path = directory / f"{robot}.jsonl"
        if not 0 < path.stat().st_size <= 8 * 1024 * 1024:
            raise ValueError("unusable lab spool")
        batches[robot] = [json.loads(line) for line in path.read_text().splitlines()]
    sink = Sources("http://127.0.0.1:1", "http://127.0.0.1:1")
    server = ObserverServer(("127.0.0.1", 0), Store(database), sink,
                            [(value, phase + name, "dag", True) for name, value in sources.items()])
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        comparisons = []
        for _ in range(2):
            for robot, rows in batches.items():
                for cursor in range(0, len(rows), 64):
                    post_events(server.server_port, rows[cursor:cursor + 64], sources[robot])
            results = []
            for task in tasks:
                trace = task["traceId"]
                robot = task["robotId"]
                actual = items(server.server_port, trace)
                expected = {row["eventId"] for row in batches[robot] if row["traceId"] == trace}
                navigate = [row["fields"] for row in actual if row["fields"].get("nodeId") == "navigate"]
                outcome = any(row.get("state") == "success" for row in navigate) if task["phase"] == "succeeded" else any(
                    row.get("errorCode") == "navigation_stopped" for row in navigate)
                if not outcome or not expected or len(actual) != len(expected) or any(
                        row["eventId"] not in expected or row["source"] != phase + robot for row in actual):
                    raise RuntimeError(f"Observer trace mismatch for {task['taskId']}")
                results.append({"taskId": task["taskId"], "traceId": trace, "actionId": task["actionId"],
                                "events": len(actual)})
            comparisons.append(results)
        if comparisons[0] != comparisons[1]:
            raise RuntimeError("a repeated import changed the trace results")
        return {"passed": True, "traces": comparisons[0], "duplicatesOnRetry": 0,
                "matchedEvents": sum(row["events"] for row in comparisons[0])}
    finally:
        server.shutdown()
        thread.join()
        server.server_close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--db", type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    print(json.dumps(check(args.run_dir, args.db), indent=2))


if __name__ == "__main__":
    main()
