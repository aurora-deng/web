"""Exercise the real gRPC executables against private SQLite ledgers and Observer."""
import argparse
import json
import os
from pathlib import Path
import secrets
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.observability.observer.server import ObserverServer
from tools.observability.observer.sources import Sources
from tools.observability.observer.store import Store
from tools.observability.scenarios import read_observer


def wait_until(get, description, seconds=10):
    stop = time.monotonic() + seconds
    while time.monotonic() < stop:
        value = get()
        if value:
            return value
        time.sleep(0.035)
    raise AssertionError(f"timed out: {description}")


class Lab:
    def __init__(self, args, directory):
        self.args = args
        self.directory = directory
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("NEBULA_FLEET_")}
        self.operator = secrets.token_hex(24)
        self.robots = {robot: secrets.token_hex(24) for robot in ("alpha", "beta", "extra")}
        self.processes = []
        self.address = "127.0.0.1:0"
        self.control = None

    @staticmethod
    def token_var(robot):
        return "NEBULA_FLEET_TEST_" + robot.upper()

    def spawn(self, *command, env=None):
        log = self.directory / f"process-{len(self.processes)}.log"
        with log.open("wb") as stdout:
            child = subprocess.Popen(command, cwd=self.directory, env=self.env if env is None else env,
                                     stdout=stdout, stderr=subprocess.STDOUT)
        self.processes.append((child, log))
        return child, log

    @staticmethod
    def stop(child):
        if child.poll() is None:
            child.kill()
            child.wait(timeout=8)

    def teardown(self, show_logs=False):
        for child, log in reversed(self.processes):
            self.stop(child)
            if show_logs and log.stat().st_size:
                print(log.read_text(encoding="utf-8", errors="replace")[-2000:], file=sys.stderr)

    def coordinator(self, nodes=2):
        grants = ["alpha", "1", self.token_var("alpha"), "beta", "2", self.token_var("beta")]
        if nodes == 3:
            grants.extend(("extra", "3", self.token_var("extra")))
        env = dict(self.env, NEBULA_FLEET_OPERATOR_TOKEN=self.operator)
        env.update({self.token_var(robot): token for robot, token in self.robots.items()})
        self.control, log = self.spawn(self.args.coordinator, self.address, "fleet.sqlite", *grants, env=env)

        def listening():
            content = log.read_text(encoding="utf-8", errors="replace")
            for line in content.splitlines():
                if line.startswith("READY "):
                    return line.removeprefix("READY ").strip()
            if self.control.poll() is not None:
                raise AssertionError(f"coordinator exited: {content}")
            return None

        self.address = wait_until(listening, "gRPC listener")

    def edge(self, robot, stations, travel=5, events=True):
        env = dict(self.env, NEBULA_FLEET_ROBOT_TOKEN=self.robots[robot])
        args = [self.args.edge, self.address, robot, f"{robot}.sqlite", str(stations), str(travel)]
        if events:
            args.append(f"{robot}.jsonl")
        return self.spawn(*args, env=env)[0]

    def drop_acceptance_echo(self, task_id):
        env = dict(self.env, NEBULA_FLEET_ROBOT_TOKEN=self.robots["alpha"])
        peer, _ = self.spawn(self.args.peer, self.address, "alpha", "alpha.sqlite", env=env)
        accepted = self.phase(task_id, "accepted")
        assert peer.poll() is None, "fault peer exited before acceptance was lost"
        self.stop(peer)
        return accepted

    def call(self, *command, denied=False):
        env = dict(self.env, NEBULA_FLEET_OPERATOR_TOKEN=self.robots["alpha"] if denied else self.operator)
        result = subprocess.run([self.args.ctl, self.address, *command], cwd=self.directory, env=env,
                                capture_output=True, encoding="utf-8", timeout=4)
        if denied:
            assert result.returncode == 2 and "rpc_status=16" in result.stderr, result.stderr
            return None
        if result.returncode:
            raise AssertionError(f"fleet CLI failed: {result.stderr}")
        try:
            return json.loads(result.stdout)
        except json.JSONDecodeError as error:
            raise AssertionError(f"fleet CLI returned invalid JSON: {result.stdout[:160]!r}; "
                                 f"diagnostics: {result.stderr[:160]!r}") from error

    def phase(self, task_id, wanted):
        def status():
            task = self.call("status", task_id)
            return task if task["phase"] == wanted else None
        return wait_until(status, f"{task_id} to reach {wanted}")


def ledger_rows(path, query):
    # The owning process must have stopped before opening its private ledger.
    connection = sqlite3.connect(path)
    try:
        return connection.execute(query).fetchall()
    finally:
        connection.close()


def observer_contract(lab, task, robot="beta"):
    work = lab.directory / f"{robot}.jsonl"

    def get_trace():
        if not work.is_file():
            return None
        rows = [json.loads(line) for line in work.read_text(encoding="utf-8").splitlines() if line.endswith("}")]
        matches = [row for row in rows if row["traceId"] == task["traceId"]]
        return matches if len(matches) == 22 else None

    events = wait_until(get_trace, "edge DAG events")
    source = secrets.token_hex(24)
    store = Store(lab.directory / "observer.sqlite")
    sources = Sources("http://127.0.0.1:1", "http://127.0.0.1:1")
    server = ObserverServer(("127.0.0.1", 0), store, sources, [(source, "fleet-lab", "dag", True)])
    observer = f"http://127.0.0.1:{server.server_port}"
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        publish = [sys.executable, "-B", str(ROOT / "scripts/project/fleet_observer.py"), str(work),
                   "--observer", observer]
        env = dict(lab.env, NEBULA_FLEET_OBSERVER_TOKEN=source)
        result = subprocess.run(publish, env=env, capture_output=True, text=True, check=True, timeout=20)
        assert json.loads(result.stdout)["eventsSent"] >= len(events)
        path = "/api/v1/traces/" + task["traceId"]
        found = read_observer(observer, path)
        states = {(row["fields"].get("nodeId"), row["fields"].get("state")) for row in found["items"]}
        assert ("navigate", "success") in states and ("report", "success") in states
        assert len(found["items"]) == len(events)
        subprocess.run(publish, env=env, capture_output=True, text=True, check=True, timeout=20)
        assert len(read_observer(observer, path)["items"]) == len(events)
    finally:
        server.shutdown()
        thread.join()
        server.server_close()


def contract(lab):
    lab.coordinator()
    lab.call("submit", "denied", "alpha", denied=True)
    beta = lab.edge("beta", 2)
    replayed = []
    # The coordinator commits acceptance, then the sender disappears before
    # recording the echo. Reconnect with the same on-disk inbox.
    for attempt in range(20):
        task_id = f"echo-{attempt}"
        initial = lab.call("submit", task_id, "alpha")
        assert initial["phase"] == "queued"
        accepted = lab.drop_acceptance_echo(task_id)
        assert accepted["robotId"] == "alpha" and accepted["epoch"] == initial["epoch"]
        if attempt == 0:
            lab.stop(lab.control)
            lab.coordinator()
            restored = lab.call("status", task_id)
            assert restored["needsReconciliation"] and restored["traceId"] == initial["traceId"]
        alpha = lab.edge("alpha", 1)
        end = lab.phase(task_id, "succeeded")
        assert end["traceId"] == initial["traceId"] and end["epoch"] == initial["epoch"]
        lab.stop(alpha)
        replayed.append(task_id)

    alpha = lab.edge("alpha", 1)
    for index in range(30):
        chosen = "alpha" if index % 2 == 0 else "beta"
        lab.call("submit", f"route-{index}", chosen)
    for index in range(30):
        task = lab.phase(f"route-{index}", "succeeded")
        assert task["robotId"] == ("alpha" if index % 2 == 0 else "beta")
    observer_contract(lab, lab.call("status", "route-1"))

    lab.stop(alpha)
    alpha = lab.edge("alpha", 1, 5000)
    lab.call("submit", "crash-executing", "alpha")
    executing = lab.phase("crash-executing", "executing")
    lab.stop(alpha)
    alpha = lab.edge("alpha", 1)
    uncertain = lab.phase("crash-executing", "uncertain")
    assert uncertain["epoch"] == executing["epoch"] and uncertain["needsReconciliation"]
    assert uncertain["outcome"] == "edge_restart"
    lab.call("submit", "held", "alpha")
    assert lab.call("status", "held")["phase"] == "queued"
    lab.stop(alpha)
    lab.stop(beta)
    lab.stop(lab.control)
    rows = dict(ledger_rows(lab.directory / "alpha.sqlite", "SELECT task_id, starts FROM inbox"))
    assert all(rows[task] == 1 for task in replayed)
    assert rows["crash-executing"] == 1 and "held" not in rows
    assert ledger_rows(lab.directory / "fleet.sqlite", "SELECT COUNT(*) FROM tasks WHERE task_id='denied'") == [(0,)]
    print("Fleet process contract passed: 30 capability routes, 20 lost acceptance echoes, "
          "coordinator and edge crash recovery, authenticated Observer replay")


def benchmark(lab):
    args = lab.args
    lab.coordinator(nodes=args.nodes)
    lab.edge("alpha", 1, events=False)
    lab.edge("beta", 2, events=False)
    if args.nodes == 3:
        lab.edge("extra", 3, events=False)
    for station in ("alpha", "beta"):
        task_id = "readiness-" + station
        lab.call("submit", task_id, station)
        lab.phase(task_id, "succeeded")
    print(f"nodes={args.nodes}, period_ms=100, simulation_ms=5, observer_spool=off", flush=True)
    subprocess.run([args.benchmark, lab.address, str(args.samples), str(args.rounds), "100"],
                   cwd=lab.directory, env=dict(lab.env, NEBULA_FLEET_OPERATOR_TOKEN=lab.operator),
                   check=True, timeout=240 + (args.samples * args.rounds // 5))


def mixed_ros_contract(lab):
    fixture, log = lab.spawn(lab.args.nav2_fixture, "--serve-alpha")
    wait_until(lambda: "SIM_ACTION_READY" in log.read_text(encoding="utf-8"), "ROS Action fixture")
    lab.coordinator()
    lab.edge("alpha", 1, "nav2")
    lab.edge("beta", 2)
    lab.call("submit", "mixed-beta", "beta")
    assert lab.phase("mixed-beta", "succeeded")["robotId"] == "beta"

    # Discovery and /clock subscription warm up independently of the gRPC
    # handshake. Distinct warm-up IDs avoid replaying rejected commands.
    for index in range(6):
        task_id = f"ros-warm-{index}"
        lab.call("submit", task_id, "alpha")

        def outcome():
            result = lab.call("status", task_id)
            return result if result["phase"] in ("succeeded", "failed", "uncertain") else None

        readiness = wait_until(outcome, "ROS edge warm-up")
        if readiness["phase"] == "succeeded":
            break
        assert readiness["phase"] == "failed" and readiness["outcome"] == "precondition_failed", readiness
        time.sleep(0.25)
    else:
        raise AssertionError("ROS edge never became ready")
    for index in range(5):
        task_id = f"ros-run-{index}"
        lab.call("submit", task_id, "alpha")
        task = lab.phase(task_id, "succeeded")
        assert task["robotId"] == "alpha" and task["outcome"] == "arrived"
    assert fixture.poll() is None
    observer_contract(lab, task, "alpha")
    print("Mixed fleet passed: 5 ROS 2 Action routes, software-only edge and Observer correlation")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("coordinator", "edge", "ctl", "peer"):
        parser.add_argument("--" + arg, type=lambda value: Path(value).absolute(), required=True)
    parser.add_argument("--benchmark", type=lambda value: Path(value).absolute())
    parser.add_argument("--nav2-fixture", type=lambda value: Path(value).absolute())
    parser.add_argument("--nodes", type=int, choices=(2, 3), default=2)
    parser.add_argument("--samples", type=int, default=1000)
    parser.add_argument("--rounds", type=int, default=3)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nebula-fleet-") as directory:
        lab = Lab(args, Path(directory))
        try:
            if args.benchmark:
                benchmark(lab)
            elif args.nav2_fixture:
                mixed_ros_contract(lab)
            else:
                contract(lab)
        except BaseException:
            lab.teardown(show_logs=True)
            raise
        finally:
            lab.teardown()


if __name__ == "__main__":
    main()
