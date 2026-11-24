"""Exercise the simulated motor safety path using independent ROS 2 processes."""

import argparse
from contextlib import ExitStack
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

import rclpy
from geometry_msgs.msg import Twist, TwistStamped
from nav_msgs.msg import Odometry
from std_msgs.msg import String, UInt64MultiArray
from std_srvs.srv import SetBool, Trigger

from .m08_probe import Probe
from .verify_m08 import require, stop_process


class Scenario(Probe):
    def __init__(self):
        super().__init__(["alpha"])
        self.command = self.create_publisher(Twist, "/alpha/cmd_vel", 1)
        self.permit = self.create_publisher(UInt64MultiArray, "/alpha/body/motion_permit", 1)
        self.direct = self.create_publisher(TwistStamped, "/alpha/actuator_sim/target_until", 1)
        self.guard = {}
        self.guard_sub = self.create_subscription(String, "/alpha/actuator_sim/status", self.on_guard, 1)
        self.measured = None
        self.odom = self.create_subscription(Odometry, "/alpha/odom", self.on_odometry, 1)
        self.grant = 0
        self.send_commands = True
        self.send_permits = True
        self.ticker = self.create_timer(0.025, self.publish_inputs)

    def on_guard(self, message):
        self.guard = json.loads(message.data)

    def on_odometry(self, message):
        velocity = message.twist.twist
        self.measured = velocity.linear.x, velocity.angular.z

    def publish_inputs(self):
        stamp = time.monotonic_ns()
        if self.send_permits:
            self.permit.publish(UInt64MultiArray(data=[self.grant, stamp, stamp + 700_000_000 if self.grant else 0]))
        if self.send_commands:
            motion = Twist()
            motion.linear.x, motion.angular.z = 2.0, 3.0
            self.command.publish(motion)

    def moving(self):
        return self.plant.get("alpha", {}).get("linearMps", 0) >= 0.15

    def neutral(self):
        plant = self.plant.get("alpha", {})
        return plant.get("linearMps") == 0 and plant.get("angularRadps") == 0

    def fixture(self, path, value):
        answer = self.call(SetBool, "/alpha/" + path, SetBool.Request(data=value))
        require(answer.success, path + " fixture command failed")

    def start_motion(self):
        self.grant = time.monotonic_ns()
        self.wait(self.moving, "new grant reached the simulated motor", 4)

    def verify_stays_stopped(self):
        before = self.plant["alpha"]["nonzeroCommands"]
        self.spin_for(0.3)
        require(self.neutral() and self.plant["alpha"]["nonzeroCommands"] == before,
                "an existing grant reapplied a target")

    def wait_for_fault(self, reason, started):
        self.wait(lambda: self.guard.get("state") == "fault" and self.guard.get("reason") == reason,
                  reason, 2)
        self.wait(self.neutral, "zero output after " + reason, 2)
        return round(1000 * (time.monotonic() - started), 1)

    def reset(self):
        answer = self.call(Trigger, "/alpha/actuator_sim/reset", Trigger.Request())
        require(answer.success, "reset was not accepted by the fixture")

    def recover(self):
        self.wait(self.neutral, "neutral simulated drive")
        self.spin_for(0.15)  # permit a fresh zero feedback sample to reach the periodic task
        self.reset()
        self.wait(lambda: self.guard.get("state") == "disarmed", "explicit reset returns disarmed", 2)
        self.verify_stays_stopped()
        self.start_motion()

    def independent_watchdog(self, process):
        start = time.monotonic()
        process.send_signal(signal.SIGSTOP)
        try:
            self.wait(lambda: self.measured == (0.0, 0.0),
                      "separate simulated drive timed out while controller was suspended", 2)
            stopped = round(1000 * (time.monotonic() - start), 1)
            require(stopped <= 400, "watchdog stop not observed within the 400 ms software lab budget")

            message = TwistStamped()
            # Exercise the drive limits directly, bypassing the paused L1 controller.
            now = time.monotonic_ns()
            message.header.stamp.sec, message.header.stamp.nanosec = divmod(now + 500_000_000, 1_000_000_000)
            message.twist.linear.x, message.twist.angular.z = 8.0, -8.0
            self.direct.publish(message)
            self.wait(lambda: self.measured == (0.4, -1.5), "drive enforced L2 limits", 2)
            self.wait(lambda: self.measured == (0.0, 0.0), "drive expired a target with remaining sender validity", 2)
        finally:
            process.send_signal(signal.SIGCONT)
        self.wait(lambda: self.guard.get("state") == "disarmed", "paused controller returned disarmed", 2)
        self.verify_stays_stopped()
        self.start_motion()
        return {"zeroObservedByMs": stopped, "l2ClampedMps": 0.4, "l2ClampedRadps": -1.5,
                "oldGrantDidNotResume": True}


def spawn(stack, directory, name, *argv):
    with (directory / f"{name}.log").open("wb") as log:
        process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    stack.callback(stop_process, process)
    return process


def run(args):
    with ExitStack() as stack:
        spawn(stack, args.run_dir, "plant", sys.executable, str(Path(__file__).with_name("kinematic_lab.py")),
              "--guarded", "alpha")
        process = spawn(stack, args.run_dir, "actuator", args.actuator, "--ros-args", "-r", "__ns:=/alpha")
        probe = Scenario()
        stack.callback(probe.destroy_node)
        probe.wait(lambda: probe.neutral() and probe.guard.get("state") == "disarmed" and
                   probe.direct.get_subscription_count() == 1, "simulated motor and guard are connected")
        probe.verify_stays_stopped()
        probe.start_motion()
        probe.wait(lambda: probe.plant["alpha"]["linearMps"] >= 0.345, "linear limit reached", 3)
        probe.spin_for(0.25)
        plant = probe.plant["alpha"]
        require(0.345 <= plant["linearMps"] <= 0.350001 and
                1.295 <= plant["angularRadps"] <= 1.300001, "L1 limits outside expected bounds")
        bounds = {"linearMps": plant["linearMps"], "angularRadps": plant["angularRadps"]}

        probe.send_commands = False
        stale_ms = probe.wait_for_fault("command_timeout", time.monotonic())
        probe.send_commands = True
        probe.recover()

        start = time.monotonic()
        probe.fixture("plant/feedback", False)
        feedback_ms = probe.wait_for_fault("feedback_timeout", start)
        probe.reset()
        probe.spin_for(0.2)
        require(probe.guard.get("state") == "fault", "reset succeeded without fresh feedback")
        probe.fixture("plant/feedback", True)
        probe.recover()

        start = time.monotonic()
        probe.fixture("actuator_sim/emergency", True)
        emergency_ms = probe.wait_for_fault("emergency_stop", start)
        probe.reset()
        probe.spin_for(0.2)
        require(probe.guard.get("state") == "fault", "reset succeeded with emergency stop held")
        probe.fixture("actuator_sim/emergency", False)
        probe.recover()

        probe.send_permits = False
        permit_ms = probe.wait_for_fault("permit_timeout", time.monotonic())
        probe.send_permits = True
        probe.recover()
        watchdog = probe.independent_watchdog(process)

        process.send_signal(signal.SIGINT)
        require(process.wait(timeout=6) == 0, "old actuator did not exit cleanly")
        probe.wait(probe.neutral, "plant stopped after the controller exited")
        probe.guard.clear()
        spawn(stack, args.run_dir, "actuator-restarted", args.actuator, "--ros-args", "-r", "__ns:=/alpha")
        probe.wait(lambda: probe.guard.get("state") == "disarmed", "controller restarted disarmed")
        probe.verify_stays_stopped()
        probe.start_motion()

        return {"schema": 1, "simulation": True, "passed": True,
                "rosDomain": int(os.environ["ROS_DOMAIN_ID"]), "l1Observed": bounds,
                "stoppedObservedByMs": {"commandTimeout": stale_ms, "feedbackTimeout": feedback_ms,
                                        "emergencyStop": emergency_ms, "permitTimeout": permit_ms},
                "watchdog": watchdog, "restartDidNotResumeOldGrant": True, "finalStatus": probe.guard}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--actuator", type=Path, required=True)
    parser.add_argument("--run-dir", type=Path, required=True)
    args = parser.parse_args()
    if os.environ.get("ROS_LOCALHOST_ONLY") != "1" or not os.environ.get("ROS_DOMAIN_ID", "").isdigit():
        parser.error("set ROS_LOCALHOST_ONLY=1 and a dedicated ROS_DOMAIN_ID")
    args.actuator = args.actuator.resolve(strict=True)
    args.run_dir = args.run_dir.resolve()
    os.umask(0o077)
    args.run_dir.mkdir(mode=0o700)
    os.environ.update(ROS_HOME=str(args.run_dir / "ros-home"), ROS_LOG_DIR=str(args.run_dir / "ros-log"))
    rclpy.init()
    try:
        result = run(args)
        evidence = args.run_dir / "m09-ros-safety.json"
        evidence.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(evidence)
    finally:
        rclpy.shutdown()


if __name__ == "__main__":
    main()
