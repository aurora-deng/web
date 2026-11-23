"""Exercise the real gRPC executables against private SQLite ledgers and Observer."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
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
from tools.observability.observer.store import Capacity, Store
from tools.observability.scenarios import post, read_observer
from tools.agent.episode_recording import recording_events, replay
from tools.agent.local_process_metrics import sample as process_metrics
from tools.agent.simulation_models import InferenceLane, percentile, train_fixture


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
        self.agent_tokens = {role: secrets.token_hex(24) for role in ("READ", "DRAFT", "REVIEW")}
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

    def coordinator(self, nodes=2, *, agent_sim=False):
        grants = ["alpha", "1", self.token_var("alpha"), "beta", "2", self.token_var("beta")]
        if nodes == 3:
            grants.extend(("extra", "3", self.token_var("extra")))
        env = dict(self.env, NEBULA_FLEET_OPERATOR_TOKEN=self.operator)
        env.update({self.token_var(robot): token for robot, token in self.robots.items()})
        if agent_sim:
            env.update({f"NEBULA_FLEET_AGENT_{role}_TOKEN": token for role, token in self.agent_tokens.items()})
            grants.insert(0, "--agent-sim")
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
        peer, log = self.spawn(self.args.peer, self.address, "alpha", "alpha.sqlite", env=env)

        def accepted():
            if peer.poll() is not None:
                raise AssertionError(f"fault peer stopped before acceptance was lost: {log.read_text(encoding='utf-8')}")
            task = self.call("status", task_id)
            return task if task["phase"] == "accepted" else None

        task = wait_until(accepted, f"{task_id} to persist acceptance")
        self.stop(peer)
        return task

    def call(self, *command, denied=False, credential=None):
        token = credential or (self.robots["alpha"] if denied else self.operator)
        env = dict(self.env, NEBULA_FLEET_OPERATOR_TOKEN=token)
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

    def agent(self, *command, denied=False, code=0, roles=None, flags=()):
        env = dict(self.env)
        if roles is None:
            env["NEBULA_FLEET_OPERATOR_TOKEN"] = self.robots["alpha"] if denied else self.operator
        else:
            env.update({f"NEBULA_FLEET_AGENT_{role}_TOKEN": self.agent_tokens[credential]
                        for role, credential in roles.items()})
        result = subprocess.run([self.args.agent, "--events", *flags, self.address, *command], cwd=self.directory, env=env,
                                capture_output=True, encoding="utf-8", timeout=4)
        assert result.returncode == code, f"agent exit {result.returncode}: {result.stdout[:240]} {result.stderr[:240]}"
        return json.loads(result.stdout), result.stderr


def ledger_rows(path, query):
    # The owning process must have stopped before opening its private ledger.
    connection = sqlite3.connect(path)
    try:
        return connection.execute(query).fetchall()
    finally:
        connection.close()


def observer_replay(lab, work, trace_id, event_count):
    def get_trace():
        if not work.is_file():
            return None
        rows = [json.loads(line) for line in work.read_text(encoding="utf-8").splitlines() if line.endswith("}")]
        matches = [row for row in rows if row["traceId"] == trace_id]
        return matches if len(matches) == event_count else None

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
        path = "/api/v1/traces/" + trace_id
        found = read_observer(observer, path)
        states = {(row["fields"].get("nodeId"), row["fields"].get("state")) for row in found["items"]}
        assert len(found["items"]) == len(events)
        subprocess.run(publish, env=env, capture_output=True, text=True, check=True, timeout=20)
        assert len(read_observer(observer, path)["items"]) == len(events)
        return states
    finally:
        server.shutdown()
        thread.join()
        server.server_close()


def observer_contract(lab, task, robot="beta"):
    states = observer_replay(lab, lab.directory / f"{robot}.jsonl", task["traceId"], 22)
    assert ("navigate", "success") in states and ("report", "success") in states


def agent_contract(lab, queued, uncertain):
    task_id = queued["taskId"]
    proposal, events = lab.agent("propose", task_id)
    assert proposal["planner"] == "rules" and proposal["proposal"] == {
        "skillId": "fleet.propose_sim_reschedule", "version": 1, "evidenceTaskIds": [task_id],
    }
    assert proposal["evidence"][0]["traceId"] == queued["traceId"]
    assert lab.call("status", task_id) == queued

    path = lab.directory / "agent.jsonl"
    path.write_text(events, encoding="utf-8")
    states = observer_replay(lab, path, proposal["traceId"], 17)
    assert ("read_task_status", "success") in states and ("verify_proposal", "success") in states
    assert queued["traceId"] not in events and '"taskId"' not in events

    other_id = uncertain["taskId"]
    review, _ = lab.agent("review", other_id, "route-1")
    assert review["proposal"]["skillId"] == "fleet.review_failures"
    assert review["proposal"]["evidenceTaskIds"] == [other_id]
    assert review["evidence"][0]["traceId"] == uncertain["traceId"]
    assert review["evidence"][0]["outcome"] == "edge_restart"

    rejected, _ = lab.agent("propose", other_id, code=2)
    assert rejected["errorCode"] == "task_not_eligible" and rejected["proposal"] is None
    denied, _ = lab.agent("review", other_id, denied=True, code=2)
    assert denied["errorCode"] == "evidence_denied" and denied["evidence"] == [] and denied["proposal"] is None
    assert lab.call("status", other_id)["phase"] == "uncertain"
    print("Fleet Agent process contract passed: authenticated reads, bounded proposals, Observer replay")


def approved_handoff(lab):
    read = {"READ": "READ"}
    draft_role = {**read, "DRAFT": "DRAFT"}
    reviewer = {"REVIEW": "REVIEW"}
    lab.coordinator(agent_sim=True)
    lab.call("submit", "denied-by-role", "alpha", denied=True, credential=lab.agent_tokens["READ"])
    original = lab.call("submit", "before-approval", "alpha")
    bad, _ = lab.agent("stage", original["taskId"], code=2, roles=draft_role,
                       flags=("--candidate", "robot.navigate", "1", original["taskId"]))
    assert bad["errorCode"] == "proposal_rejected"
    draft, planned = lab.agent("stage", original["taskId"], roles=draft_role)
    proposal_id = draft["proposalId"]
    assert draft["state"] == "pending" and draft["sourceEpoch"] == original["epoch"]
    assert draft["station"] == "alpha" and draft["expiresAt"] - draft["createdAt"] == 300
    again, _ = lab.agent("stage", original["taskId"], roles=draft_role, flags=("--trace-id", proposal_id))
    assert again == draft and lab.call("status", original["taskId"]) == original
    denied, _ = lab.agent("approve", proposal_id, roles={"REVIEW": "DRAFT"}, code=2)
    assert denied["errorCode"] == "review_denied"

    lab.stop(lab.control)
    lab.coordinator(agent_sim=True)
    assert lab.agent("inspect", proposal_id, roles=reviewer)[0] == draft
    with ThreadPoolExecutor(max_workers=2) as pool:
        outcomes = list(pool.map(lambda _: lab.agent("approve", proposal_id, roles=reviewer), range(2)))
    approved, audited = outcomes[0]
    assert approved == outcomes[1][0] and approved["state"] == "approved"
    replacement = approved["replacementTaskId"]
    assert lab.call("status", original["taskId"])["outcome"] == "agent_superseded"
    assert lab.call("status", replacement)["traceId"] == proposal_id

    lab.stop(lab.control)
    lab.coordinator(agent_sim=True)
    assert lab.agent("approve", proposal_id, roles=reviewer)[0] == approved
    lab.edge("alpha", 1)
    finished = lab.phase(replacement, "succeeded")
    assert finished["traceId"] == proposal_id and finished["outcome"] == "arrived"
    events_file = lab.directory / "approval-trace.jsonl"
    alpha_spool = lab.directory / "alpha.jsonl"
    edge_events = wait_until(lambda: "\"state\":\"success\",\"workflow\":" in alpha_spool.read_text(encoding="utf-8")
                             if alpha_spool.is_file() else None, "approved edge trace")
    assert edge_events
    events_file.write_text(planned + audited + alpha_spool.read_text(encoding="utf-8"), encoding="utf-8")
    states = observer_replay(lab, events_file, proposal_id, 49)
    assert {("verify_proposal", "success"), ("stage_review", "success"), ("approve_review", "success"),
            ("navigate", "success")} <= states

    beta = lab.call("submit", "approval-race", "beta")
    stale, _ = lab.agent("stage", beta["taskId"], roles=draft_role)
    lab.edge("beta", 2)
    lab.phase(beta["taskId"], "succeeded")
    halted, _ = lab.agent("approve", stale["proposalId"], roles=reviewer, code=2)
    assert halted["state"] == "stale" and not halted["replacementTaskId"]
    lab.teardown()
    assert ledger_rows(lab.directory / "alpha.sqlite", "SELECT task_id,starts FROM inbox") == [(replacement, 1)]
    assert ledger_rows(lab.directory / "fleet.sqlite", "SELECT COUNT(*) FROM tasks") == [(3,)]
    print("Agent approval handoff passed: distinct roles, durable approval, single edge start, shared trace, stale rejection")


def model_adapter_contract(lab):
    lab.coordinator(agent_sim=True)
    task_id = "model-plan"
    lab.call("submit", task_id, "alpha")
    good = {"skillId": "fleet.propose_sim_reschedule", "version": 1, "evidenceTaskIds": [task_id]}
    running_case = "valid"
    requests = []

    class OllamaFixture(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            assert self.path == "/api/chat"
            requests.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
            if running_case == "offline":
                self.send_error(503)
                return
            if running_case == "slow":
                time.sleep(1.5)
            candidate = good if running_case != "fake-reference" else {**good, "evidenceTaskIds": ["invented-id"]}
            content = "bad candidate" if running_case == "malformed" else json.dumps(candidate)
            answer = json.dumps({"done": True, "message": {"content": content}, "eval_count": 20}).encode()
            try:
                self.send_response(200)
                self.send_header("Content-Length", str(len(answer)))
                self.end_headers()
                self.wfile.write(answer)
            except OSError:
                pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), OllamaFixture)
    serving = threading.Thread(target=server.serve_forever, daemon=True)
    serving.start()

    def model(action, *, code=0, timeout="4", events=False):
        env = dict(lab.env, NEBULA_FLEET_AGENT_READ_TOKEN=lab.agent_tokens["READ"],
                   NEBULA_FLEET_AGENT_DRAFT_TOKEN=lab.agent_tokens["DRAFT"])
        result = subprocess.run([sys.executable, "-B", str(ROOT / "tools/agent/ollama_planner.py"),
                                 "--agent", lab.args.agent, "--endpoint", lab.address, "--model", "fixture:local",
                                 "--ollama-port", str(server.server_port), "--timeout", timeout,
                                 *(("--events",) if events else ()), action, task_id],
                                cwd=lab.directory, env=env, capture_output=True, encoding="utf-8", timeout=12)
        assert result.returncode == code, f"model planner returned {result.returncode}: {result.stdout[:240]}"
        answer = json.loads(result.stdout)
        if events:
            path = lab.directory / "model-trace.jsonl"
            path.write_text(result.stderr, encoding="utf-8")
            assert ("model_candidate", "failed") in observer_replay(lab, path, answer["traceId"], 21)
            assert "bad candidate" not in result.stderr
        return answer

    try:
        pending = model("stage")
        assert pending["modelStatus"] == "candidate_checked" and pending["sameAsRuleDraft"] and pending["state"] == "pending"
        assert pending["eval_count"] == 20
        sent = requests[0]
        assert sent["format"]["additionalProperties"] is False and sent["stream"] is False
        private_input = json.loads(sent["messages"][1]["content"])["tasks"][0]
        assert set(private_input) == {"taskId", "phase", "needsReconciliation", "outcome"}

        running_case = "fake-reference"
        assert model("stage", code=2)["errorCode"] == "proposal_rejected"
        running_case = "malformed"
        assert model("stage", code=2, events=True)["errorCode"] == "model_output_invalid"
        running_case = "offline"
        assert model("propose")["modelStatus"] == "rules_fallback"
        running_case = "slow"
        assert model("propose", timeout="0.8")["modelStatus"] == "rules_fallback"
        assert lab.call("status", task_id)["phase"] == "queued"
    finally:
        server.shutdown()
        serving.join()
        server.server_close()
    lab.teardown()
    assert ledger_rows(lab.directory / "fleet.sqlite", "SELECT COUNT(*) FROM agent_sim_reviews") == [(1,)]
    assert ledger_rows(lab.directory / "fleet.sqlite", "SELECT COUNT(*) FROM tasks") == [(1,)]
    print("Ollama adapter HTTP fixture passed: valid draft, unchecked IDs denied, invalid JSON denied, outage and timeout fallback")


def backpressure_contract(lab):
    lab.coordinator()
    robot_env = dict(lab.env, NEBULA_FLEET_ROBOT_TOKEN=lab.robots["alpha"])
    subprocess.run([lab.args.peer, "--versions", lab.address, "alpha"], env=robot_env, check=True, timeout=8)
    env = dict(lab.env, NEBULA_FLEET_OPERATOR_TOKEN=lab.operator)
    readers = [lab.spawn(lab.args.peer, "--slow-watch", lab.address, env=env) for _ in range(2)]
    wait_until(lambda: lab.call("load")["watchers"] == 2, "two slow state watchers", seconds=6)
    subprocess.run([lab.args.peer, "--watch-full", lab.address], env=env, check=True, timeout=8)
    assert lab.call("load")["watchRejections"] == 1

    lab.edge("alpha", 1, events=False)
    for index in range(30):
        lab.call("submit", f"slow-watch-{index}", "alpha")
    for index in range(30):
        lab.phase(f"slow-watch-{index}", "succeeded")
    stats = []
    for process, log in readers:
        assert process.wait(timeout=40) == 0
        stats.append(json.loads(log.read_text(encoding="utf-8")))
    def drained():
        load = lab.call("load")
        return load if load["watchers"] == 0 else None

    final = wait_until(drained, "all status streams released")
    assert final["queued"] == final["active"] == 0
    print(json.dumps({"controlCompleted": 30, "slowSecondsPerWatcher": 30, "watchers": stats,
                      "refusedStreams": final["watchRejections"], "activeWatchersAfterClose": final["watchers"]}))


def simulation_models_contract(lab):
    models = lab.directory / "simulation"
    report = train_fixture(models)
    assert report["rollbackReproduced"] and report["audit"] == [("activate", "grid-1"), ("activate", "grid-2"), ("rollback", "grid-1")]
    archive = models / "episodes"
    manifest, _, quality = replay(archive)
    assert replay(archive)[2] == quality and quality["rejected"] == {"missing_frame": 2, "low_clock_quality": 9}
    diagnostic = quality["sequence"]["diagnostics"]
    assert diagnostic["range"] == {"received": 899, "unique": 898, "duplicate": 1, "late": 1, "gaps": 2}
    for streams in quality["sequence"].values():
        assert all(item["received"] == item["unique"] + item["duplicate"] for item in streams.values())
    bad = models / "corrupt-copy"
    bad.mkdir()
    (bad / "manifest.json").write_bytes((archive / "manifest.json").read_bytes())
    (bad / "frames.jsonl").write_bytes((archive / "frames.jsonl").read_bytes() + b"\n")
    try:
        replay(bad)
    except ValueError:
        pass
    else:
        raise AssertionError("replay did not reject a corrupted recording")

    token = secrets.token_hex(24)
    store = Store(lab.directory / "recording-observer.sqlite")
    server = ObserverServer(("127.0.0.1", 0), store, Sources("http://127.0.0.1:1", "http://127.0.0.1:1"),
                            [(token, "synthetic-recording", "device", True)])
    observer = f"http://127.0.0.1:{server.server_port}"
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    events = recording_events(manifest, quality, time.time_ns())
    try:
        path = "/ingest/v1/events"
        assert post(observer, path, events, token) == {"inserted": 3, "duplicates": 0}
        assert post(observer, path, events, token) == {"inserted": 0, "duplicates": 3}
        trace = read_observer(observer, "/api/v1/traces/" + events[0]["traceId"])
        assert len(trace["items"]) == 3 and all(item["simulated"] for item in trace["items"])
        assert any(item["fields"].get("recordingId") == manifest["recordingId"] for item in trace["items"])
    finally:
        server.shutdown()
        server.server_close()
        worker.join()

    bounded = Store(lab.directory / "limited-observer.sqlite", max_bytes=1024**2)
    admitted = 0
    batch_size = 128
    for offset in range(0, 8192, batch_size):
        batch = [{**events[0], "eventId": f"cap-{index}"} for index in range(offset, offset + batch_size)]
        try:
            appended = bounded.append("synthetic-recording", "device", True, batch)
        except Capacity:
            break
        assert appended == {"inserted": batch_size, "duplicates": 0}
        admitted += batch_size
    else:
        raise AssertionError("Observer did not enforce its storage limit")
    assert admitted > 0
    with bounded.read() as db:
        assert db.execute("SELECT count(*) FROM events").fetchone()[0] == admitted

    lab.coordinator()
    lab.edge("alpha", 1, events=False)

    def controls(group, count=14):
        elapsed = []
        for index in range(count):
            key = f"{group}-{index}"
            began = time.monotonic()
            lab.call("submit", key, "alpha")
            task = lab.phase(key, "succeeded")
            assert task["outcome"] == "arrived"
            elapsed.append((time.monotonic() - began) * 1000)
        return {"operations": count, "p95Ms": percentile(elapsed, 0.95), "p99Ms": percentile(elapsed, 0.99)}

    steady = controls("baseline")
    with InferenceLane(models / "models.sqlite") as lane:
        observation = {"remaining": 7, "clearance": 88, "ageMs": 12}
        slow = lane.offer({**observation, "fixtureDelayMs": 1800}, 0.9)
        stale = lane.offer({**observation, "ageMs": 160}, 4)
        assert slow is not None and stale is not None and lane.offer(observation, 3) is None
        assert not slow.done(), "simulation timeout fixture unexpectedly completed before load"
        stressed = controls("worker-busy")
        assert slow.result(timeout=2) == {"state": "timeout", "step": 0}
        refused = stale.result(timeout=2)
        assert refused == {"state": "ready", "version": "grid-1", "step": 0, "refused": True}
        assert lane.offer(observation, 3).result(timeout=4)["step"] == 1
    report.update({"replay": {"windowSeconds": 90, "range": diagnostic["range"], "replayedDigestsMatch": True,
                             "actionSha256": quality["actionSha256"], "observerReplayed": 3,
                             "observerCapacityReachedAt": admitted},
                   "isolation": {"baseline": steady, "busy": stressed, "workerTimeout": True,
                                 "overloadRefusal": True, "observerStopped": True}})
    print("SIMULATION_EVIDENCE " + json.dumps(report, separators=(",", ":")))


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
    queued = lab.call("submit", "held", "alpha")
    assert queued["phase"] == "queued"
    if lab.args.agent:
        agent_contract(lab, queued, uncertain)
    lab.stop(alpha)
    lab.stop(beta)
    lab.stop(lab.control)
    rows = dict(ledger_rows(lab.directory / "alpha.sqlite", "SELECT task_id, starts FROM inbox"))
    assert all(rows[task] == 1 for task in replayed)
    assert rows["crash-executing"] == 1 and "held" not in rows
    assert ledger_rows(lab.directory / "fleet.sqlite", "SELECT COUNT(*) FROM tasks WHERE task_id='denied'") == [(0,)]
    assert ledger_rows(lab.directory / "fleet.sqlite", "SELECT COUNT(*) FROM tasks") == [(52,)]
    print("Fleet process contract passed: 30 capability routes, 20 lost acceptance echoes, "
          "coordinator and edge crash recovery, authenticated Observer replay")


def benchmark(lab):
    args = lab.args
    lab.coordinator(nodes=args.nodes)
    actors = {"coordinator": lab.control, "alpha": lab.edge("alpha", 1, events=False),
              "beta": lab.edge("beta", 2, events=False)}
    if args.nodes == 3:
        actors["extra"] = lab.edge("extra", 3, events=False)
    for station in ("alpha", "beta"):
        task_id = "readiness-" + station
        lab.call("submit", task_id, station)
        lab.phase(task_id, "succeeded")
    print(f"nodes={args.nodes}, period_ms=100, simulation_ms=5, observer_spool=off", flush=True)
    command = [args.benchmark, lab.address, str(args.samples), str(args.rounds), "100"]
    child, log = lab.spawn(*command, env=dict(lab.env, NEBULA_FLEET_OPERATOR_TOKEN=lab.operator))
    started_at = time.monotonic()
    initial = {name: process_metrics(process.pid) for name, process in actors.items()}
    resources = {name: {"peakSampleRssBytes": item["rssBytes"], "cpuSeconds": 0} for name, item in initial.items()}
    queue = []
    terminal = started_at + 240 + (args.samples * args.rounds // 5)
    while child.poll() is None:
        if time.monotonic() > terminal:
            raise TimeoutError("Fleet capacity probe timed out")
        load = lab.call("load")
        queue.append(load["queued"])
        for name, process in actors.items():
            current = process_metrics(process.pid)
            observed = resources[name]
            observed["peakSampleRssBytes"] = max(observed["peakSampleRssBytes"], current["rssBytes"])
            observed["cpuSeconds"] = round(current["cpuSeconds"] - initial[name]["cpuSeconds"], 6)
        try:
            child.wait(timeout=1)
        except subprocess.TimeoutExpired:
            pass
    output = log.read_text(encoding="utf-8")
    print(output, end="")
    if child.returncode:
        raise AssertionError(f"Fleet benchmark exited with {child.returncode}")
    rows = list(csv.DictReader(output.splitlines()))
    assert len(rows) == args.rounds and all(int(row["accepted"]) == args.samples for row in rows)
    if args.report:
        report = {"nodes": args.nodes, "periodMs": 100, "navigationMs": 5, "elapsedSeconds": time.monotonic() - started_at,
                  "rounds": rows, "resourceSamples": len(queue), "peakSampleQueue": max(queue, default=0),
                  "processes": resources}
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


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
    for arg in ("coordinator", "edge", "ctl", "peer", "agent"):
        parser.add_argument("--" + arg, type=lambda value: Path(value).absolute(), required=arg != "agent")
    parser.add_argument("--benchmark", type=lambda value: Path(value).absolute())
    parser.add_argument("--nav2-fixture", type=lambda value: Path(value).absolute())
    parser.add_argument("--agent-approval", action="store_true")
    parser.add_argument("--agent-model", action="store_true")
    parser.add_argument("--backpressure", action="store_true")
    parser.add_argument("--simulation-models", action="store_true")
    parser.add_argument("--nodes", type=int, choices=(2, 3), default=2)
    parser.add_argument("--samples", type=int, default=1000)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nebula-fleet-") as directory:
        lab = Lab(args, Path(directory))
        try:
            if args.benchmark:
                benchmark(lab)
            elif args.agent_approval:
                approved_handoff(lab)
            elif args.agent_model:
                model_adapter_contract(lab)
            elif args.backpressure:
                backpressure_contract(lab)
            elif args.simulation_models:
                simulation_models_contract(lab)
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
