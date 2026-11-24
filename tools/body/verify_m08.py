"""Verify the M08 ROS edge against an isolated Fleet coordinator on a local forwarded port."""
import argparse
from contextlib import ExitStack, closing
import json
import math
import os
from pathlib import Path
import re
import signal
import sqlite3
import subprocess
import sys
import time

from ament_index_python.packages import get_package_share_directory
import rclpy
from action_msgs.msg import GoalStatus
from lifecycle_msgs.msg import Transition
from std_srvs.srv import Trigger
import yaml

from .m08_link import TestLink
from .m08_probe import Probe, callback_isolation


ROBOTS = {"alpha": (1, 0.5, 0.0), "beta": (2, 1.0, 0.3)}
PHASES_DONE = {"succeeded", "failed", "uncertain"}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def token(path):
    require(path.stat().st_mode & 0o077 == 0, "use private test token files")
    value = path.read_text().strip()
    require(re.fullmatch(r"[a-zA-Z0-9_-]{24,128}", value), "invalid test token")
    return value


def nav2_parameters(target, robot, guarded=False):
    source = Path(get_package_share_directory("nav2_bringup")) / "params/nav2_params.yaml"
    params = yaml.safe_load(source.read_text())
    navigator = params["bt_navigator"]["ros__parameters"]
    # The two simulation stacks share a VM. Give a scheduled planner or
    # costmap service time to acknowledge its goal before the behavior tree retries.
    navigator.update(odom_topic=f"/{robot}/odom", bt_loop_duration=50, default_server_timeout=1000)
    controller = params["controller_server"]["ros__parameters"]
    controller["odom_topic"] = f"/{robot}/odom"
    controller["controller_frequency"] = 10.0
    controller["progress_checker"].update(required_movement_radius=0.10, movement_time_allowance=12.0)
    controller["general_goal_checker"].update(xy_goal_tolerance=0.08, yaw_goal_tolerance=0.12)
    controller["FollowPath"].update(xy_goal_tolerance=0.08, vx_samples=10, vy_samples=1, vtheta_samples=15)
    for key in ("global_costmap", "local_costmap"):
        layer = params[key][key]["ros__parameters"]
        layer.setdefault("static_layer", {}).update(
            map_topic=f"/{robot}/map", map_subscribe_transient_local=True)
        for plugin in ("voxel_layer", "obstacle_layer"):
            if plugin in layer and "scan" in layer[plugin]:
                layer[plugin]["scan"]["topic"] = f"/{robot}/scan"
    if guarded:
        # The guarded fixture is a small static map on a two-vCPU lab VM.
        controller["FollowPath"].update(vx_samples=7, vtheta_samples=9, sim_time=1.3)
        params["velocity_smoother"]["ros__parameters"]["smoothing_frequency"] = 25.0
        local = params["local_costmap"]["local_costmap"]["ros__parameters"]
        local.update(update_frequency=3.0, publish_frequency=1.0)
        global_map = params["global_costmap"]["global_costmap"]["ros__parameters"]
        global_map["publish_frequency"] = 0.5
    target.write_text(yaml.safe_dump(params, sort_keys=False))


def stop_process(process):
    # The launch runner owns all its ROS child processes as a process group.
    try:
        os.killpg(process.pid, signal.SIGINT)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=4)


class Lab:
    def __init__(self, args, stack):
        self.args, self.stack = args, stack
        self.directory = args.run_dir.resolve()
        self.bin = args.build.resolve() / "fleet/transport/grpc"
        self.robots = {"alpha": ROBOTS["alpha"]} if args.single_body else ROBOTS
        self.env = {**os.environ, "ROS_LOG_DIR": str(self.directory / "ros-log"),
                    "ROS_HOME": str(self.directory / "ros-home")}
        self.operator = token(args.tokens / "operator.token")
        self.probe = Probe(self.robots)
        self.actuator_resets = []
        stack.callback(self.probe.destroy_node)
        self.links = {robot: stack.enter_context(TestLink(args.upstream_port)) for robot in self.robots}
        self.processes = {}

    def spawn(self, name, *argv, **extra):
        path = self.directory / f"{name}.log"
        with path.open("wb") as log:
            process = subprocess.Popen(argv, env={**self.env, **extra}, stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
        self.stack.callback(stop_process, process)
        self.processes[name] = process
        return process

    def ctl(self, *argv):
        output = subprocess.run([self.bin / "nebula_fleet_ctl", f"127.0.0.1:{self.args.upstream_port}", *argv],
                                env={**self.env, "NEBULA_FLEET_OPERATOR_TOKEN": self.operator},
                                capture_output=True, text=True, timeout=6)
        require(output.returncode == 0, f"coordinator {argv[0]} failed: {output.stderr.strip()}")
        return json.loads(output.stdout)

    def launch(self):
        root = Path(__file__).resolve().parent
        plant = root / "kinematic_lab.py"
        if self.args.actuator:
            origin = str(time.monotonic_ns())
            for robot in self.robots:
                flags = ("--passive-clock",) if robot != "alpha" else ()
                self.spawn(f"kinematic-{robot}", sys.executable, plant, "--guarded", "--clock-origin-ns",
                           origin, *flags, robot)
                self.probe.watch_guard(robot)
        else:
            self.spawn("kinematic", sys.executable, plant, *self.robots)
        self.probe.wait(lambda: set(self.probe.plant) == set(self.robots), "simulated bodies")
        for robot, (stations, _, _) in self.robots.items():
            if self.args.actuator:
                self.spawn(f"{robot}-actuator", self.args.actuator, "--ros-args", "-r", f"__ns:=/{robot}")
            params = self.directory / f"{robot}-params.yaml"
            nav2_parameters(params, robot, guarded=bool(self.args.actuator))
            self.spawn(f"{robot}-nav2", "ros2", "launch", str(root / "nav2_lab.launch.py"),
                       f"robot:={robot}", f"params:={params}")
            edge_env = {"NEBULA_FLEET_ROBOT_TOKEN": token(self.args.tokens / f"{robot}.token")}
            self.spawn(f"{robot}-edge", self.bin / "nebula_fleet_edge", self.links[robot].endpoint,
                       robot, self.directory / f"{robot}.sqlite", str(stations), "nav2-managed",
                       self.directory / f"{robot}.jsonl", **edge_env)
        self.probe.wait(lambda: all(self.probe.nav2_ready(robot) for robot in self.robots),
                        "Nav2 navigators active", 45)
        self.probe.wait(lambda: self.ctl("load")["connected"] == len(self.robots), "authenticated edge streams")
        self.probe.wait(lambda: all(self.probe.body.get(robot, {}).get("lifecycle") == "unconfigured"
                                    for robot in self.robots), "unconfigured managed bodies")

    def prepare_actuator(self, robot, scenario):
        if not self.args.actuator:
            return
        probe = self.probe
        probe.wait(lambda: probe.actuator_status.get(robot, {}).get("state") in ("disarmed", "fault"),
                   f"{robot} actuator settled before {scenario}")
        settled = probe.actuator_status[robot]
        if settled["state"] == "fault":
            probe.wait(lambda: probe.plant[robot]["linearMps"] == probe.plant[robot]["angularRadps"] == 0,
                       "operator reset requires zero measured speed")
            response = probe.call(Trigger, f"/{robot}/actuator_sim/reset", Trigger.Request())
            require(response.success, "operator fixture reset was refused")
            probe.wait(lambda: probe.actuator_status[robot].get("state") == "disarmed", "explicit simulation reset")
            self.actuator_resets.append({"robotId": robot, "scenario": scenario, "reason": settled["reason"]})

    def phase(self, task):
        next_poll = 0.0

        def done():
            nonlocal next_poll
            now = time.monotonic()
            if now < next_poll:
                return None
            next_poll = now + 0.2
            status = self.ctl("status", task)
            return status if status["phase"] in PHASES_DONE else None
        return self.probe.wait(done, f"terminal task {task}", 46)

    def action_for(self, robot, task, phase):
        def matched():
            body = self.probe.body.get(robot, {})
            goal = body.get("actionId")
            if body.get("taskId") == task and goal and self.probe.actions[robot].get(goal) == phase:
                return dict(body)
        return self.probe.wait(matched, f"{robot} goal UUID and status for {task}")

    def arrived(self, robot, task):
        probe = self.probe
        probe.reset_plant(robot)
        self.prepare_actuator(robot, task)
        before = probe.plant[robot]["nonzeroCommands"]
        feedback = probe.body[robot]["feedbackCount"]
        started = time.monotonic()
        self.ctl("submit", task, robot)
        result = self.phase(task)
        require(result["robotId"] == robot and result["phase"] == "succeeded", f"navigation failed: {result}")
        ros = self.action_for(robot, task, GoalStatus.STATUS_SUCCEEDED)
        _, x, y = ROBOTS[robot]
        plant = probe.plant[robot]
        distance = math.hypot(plant["x"] - x, plant["y"] - y)
        require(ros["traceId"] == result["traceId"] and ros["feedbackCount"] > feedback and
                plant["nonzeroCommands"] > before and distance <= 0.15, f"motion proof failed for {task}")
        return {**result, "actionId": ros["actionId"], "feedback": ros["feedbackCount"] - feedback,
                "goalErrorM": round(distance, 4), "elapsedMs": round((time.monotonic() - started) * 1000, 1)}

    def rss(self, robot):
        content = Path(f"/proc/{self.processes[robot + '-edge'].pid}/status").read_text()
        return int(re.search(r"^VmRSS:\s+(\d+)", content, re.M)[1])

    def lifecycle_cycles(self):
        probe = self.probe
        baseline = self.rss("alpha")
        for _ in range(10):
            probe.change("alpha", Transition.TRANSITION_DEACTIVATE, "inactive")
            probe.change("alpha", Transition.TRANSITION_CLEANUP, "unconfigured")
            probe.change("alpha", Transition.TRANSITION_CONFIGURE, "inactive")
            probe.change("alpha", Transition.TRANSITION_ACTIVATE, "active")
            require(not probe.body["alpha"]["inFlight"], "active handles did not return to baseline")
        after = self.rss("alpha")
        require(after <= baseline * 1.05, "ten lifecycle cycles increased edge RSS by more than 5%")
        return {"cycles": 10, "rssBeforeKiB": baseline, "rssAfterKiB": after, "inFlightAfter": False}

    def hold_task(self, name):
        self.probe.reset_plant("alpha")
        self.prepare_actuator("alpha", name)
        self.probe.hold("alpha", True)
        self.ctl("submit", name, "alpha")
        body = self.action_for("alpha", name, GoalStatus.STATUS_EXECUTING)
        self.probe.wait(lambda: self.probe.plant["alpha"]["commandMoving"], "Nav2 control output active")
        return body

    def verify_cancellation(self):
        task = "m08-manual-deactivate"
        body = self.hold_task(task)
        self.probe.change("alpha", Transition.TRANSITION_DEACTIVATE, "inactive")
        cancelled = self.action_for("alpha", task, GoalStatus.STATUS_CANCELED)
        self.probe.wait(lambda: not self.probe.plant["alpha"]["commandMoving"], "velocity after local deactivation")
        result = self.phase(task)
        require(result["phase"] == "failed" and result["outcome"] == "navigation_stopped"
                and body["actionId"] == cancelled["actionId"], "local deactivation did not cancel the same goal")
        self.probe.hold("alpha", False)
        self.probe.change("alpha", Transition.TRANSITION_ACTIVATE, "active")
        return {**result, "actionId": body["actionId"], "rosActionStatus": "canceled"}

    def verify_disconnect(self):
        task = "m08-link-loss"
        body = self.hold_task(task)
        link = self.links["alpha"]
        started = time.monotonic()
        link.set_enabled(False)
        probe = self.probe
        probe.wait(lambda: probe.body["alpha"]["lifecycle"] == "inactive"
                   and probe.body["alpha"]["mode"] == "degraded"
                   and probe.body["alpha"]["reason"] == "coordinator_lease", "expired lease and degradation", 6)
        degraded_ms = round((time.monotonic() - started) * 1000, 1)
        action = self.action_for("alpha", task, GoalStatus.STATUS_CANCELED)
        probe.wait(lambda: not probe.plant["alpha"]["commandMoving"], "velocity after lost lease")
        zero_observed_ms = round((time.monotonic() - started) * 1000, 1)
        interrupted = self.ctl("status", task)
        beta_active = probe.body["beta"]["lifecycle"] == "active" if "beta" in self.robots else None
        require(interrupted["needsReconciliation"] and beta_active is not False,
                "disconnection was not isolated to the alpha edge")
        restored = time.monotonic()
        link.set_enabled(True)
        probe.wait(lambda: probe.body["alpha"]["lifecycle"] == "active", "automatic lease recovery", 5)
        reactivated_ms = round((time.monotonic() - restored) * 1000, 1)
        result = self.phase(task)
        require(result["phase"] == "failed" and result["outcome"] == "navigation_stopped"
                and interrupted["epoch"] == result["epoch"] and interrupted["traceId"] == result["traceId"]
                and action["actionId"] == body["actionId"] and body["traceId"] == result["traceId"],
                "reconciliation lost the identity of the canceled ROS Action")
        retry = self.ctl("submit", task, "alpha")
        require(retry["traceId"] == result["traceId"] and retry["phase"] == result["phase"],
                "deduplication did not retain the already-canceled task")
        probe.hold("alpha", False)
        return {**result, "actionId": action["actionId"], "degradedAfterMs": degraded_ms,
                "reactivatedAfterMs": reactivated_ms,
                "zeroObservedByMs": zero_observed_ms,
                "interruptedPhase": interrupted["phase"], "neededReconciliation": True,
                "betaStayedActive": beta_active, "rosActionStatus": "canceled"}


def run(args):
    with ExitStack() as stack:
        lab = Lab(args, stack)
        print(f"Starting {len(lab.robots)} namespaced Nav2 stack(s)", flush=True)
        lab.launch()
        probe = lab.probe
        # A durable task may reach an inactive body but cannot reach NavigateToPose.
        lab.ctl("submit", "m08-before-configure", "alpha")
        declined = lab.phase("m08-before-configure")
        require(declined["phase"] == "failed" and not probe.actions["alpha"],
                "a task reached the Action server before activation")
        for robot in lab.robots:
            probe.change(robot, Transition.TRANSITION_CONFIGURE, "inactive")
            probe.change(robot, Transition.TRANSITION_ACTIVATE, "active")
        print("Checking ROS QoS and real Nav2 simulation goals", flush=True)
        qos = probe.verify_qos("alpha", guarded=bool(args.actuator))
        goals = [lab.arrived("alpha", f"m08-alpha-{index}") for index in range(1, 2 if args.single_body else 6)]
        if not args.single_body:
            goals.append(lab.arrived("beta", "m08-beta-1"))
            require(not (set(probe.actions["alpha"]) & set(probe.actions["beta"])), "namespace Action UUIDs crossed")
        require(len({row["actionId"] for row in goals}) == len(goals), "duplicate Action UUID")
        # Drain older samples accumulated while the CLI was polling the coordinator.
        probe.spin_for(0.5)
        probe.status_times["alpha"].clear()
        probe.spin_for(5)
        times = tuple(probe.status_times["alpha"])
        require(len(times) >= 45 and times[-1][0] - times[0][0] == len(times) - 1,
                f"body status stream skipped messages ({len(times)} received in five seconds)")
        status_hz = (len(times) - 1) * 1e9 / (times[-1][1] - times[0][1])
        require(9.5 <= status_hz <= 10.5, f"body status frequency {status_hz:.2f} Hz outside the lab budget")
        lifecycle = lab.lifecycle_cycles()
        cancellation = lab.verify_cancellation()
        print("Injecting cross-host stream loss and verifying reconciliation", flush=True)
        disconnect = lab.verify_disconnect()
        recovery = lab.arrived("alpha", "m08-after-reconnect")
        callbacks = callback_isolation()
        return {"schema": 1, "passed": True, "robots": list(lab.robots), "rosDomain": int(os.environ["ROS_DOMAIN_ID"]),
                "nav2": "installed planner, controller, behavior tree and velocity smoother",
                "simulation": "local ROS kinematic plant", "guardedActuator": bool(args.actuator),
                "operatorFixtureResets": lab.actuator_resets,
                "statusHz": round(status_hz, 2),
                "statusSamples": len(times), "statusDropped": 0,
                "qos": qos, "callbacks": callbacks, "lifecycle": lifecycle, "declinedBeforeActivate": declined,
                "goals": goals, "cancellation": cancellation, "disconnect": disconnect, "recovery": recovery}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--tokens", type=Path, required=True)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--upstream-port", type=int, required=True)
    parser.add_argument("--actuator", type=Path, help="optional M09 simulator executable")
    parser.add_argument("--single-body", action="store_true", help="run the guarded cross-VM path with one simulated body")
    args = parser.parse_args()
    if os.environ.get("ROS_LOCALHOST_ONLY") != "1" or not os.environ.get("ROS_DOMAIN_ID", "").isdigit():
        parser.error("set ROS_LOCALHOST_ONLY=1 and a dedicated ROS_DOMAIN_ID")
    if not 1 <= args.upstream_port <= 65535:
        parser.error("invalid loopback port")
    if args.single_body and not args.actuator:
        parser.error("--single-body is for the M09 guarded profile")
    args.tokens = args.tokens.resolve()
    args.run_dir = args.run_dir.resolve()
    if args.actuator:
        args.actuator = args.actuator.resolve(strict=True)
    os.umask(0o077)
    args.run_dir.mkdir(mode=0o700)
    os.environ.update(ROS_LOG_DIR=str(args.run_dir / "ros-log"), ROS_HOME=str(args.run_dir / "ros-home"))
    rclpy.init()
    try:
        result = run(args)
        # A running edge owns its SQLite file exclusively.
        database = (args.run_dir / "alpha.sqlite").as_uri() + "?mode=ro"
        with closing(sqlite3.connect(database, uri=True, timeout=2)) as db:
            recovered = result["disconnect"]
            row = db.execute("SELECT starts,epoch,trace_id FROM inbox WHERE task_id=?",
                             (recovered["taskId"],)).fetchone()
        require(row == (1, recovered["epoch"], recovered["traceId"]),
                "the durable edge inbox did not retain one start with the original identity")
        recovered["edgeStarts"] = row[0]
        path = args.run_dir / ("m09-cross-vm.json" if args.actuator else "m08-evidence.json")
        path.write_text(json.dumps(result, indent=2) + "\n")
        print(f"Passed local ROS and cross-host Fleet contracts. Evidence: {path}")
    finally:
        rclpy.shutdown()


if __name__ == "__main__":
    main()
