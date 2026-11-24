"""Read the actual ROS graph, drive lifecycle services and collect M08 evidence."""
from collections import deque
import json
import threading
import time

import rclpy
from action_msgs.msg import GoalStatusArray
from lifecycle_msgs.srv import ChangeState, GetState
from rclpy.callback_groups import MutuallyExclusiveCallbackGroup
from rclpy.executors import MultiThreadedExecutor, SingleThreadedExecutor
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import LaserScan
from std_msgs.msg import String
from std_srvs.srv import SetBool, Trigger


class Probe(Node):
    def __init__(self, robots):
        super().__init__("m08_probe")
        self.body = {}
        self.plant = {}
        self.actuator_status = {}
        self.actions = {robot: {} for robot in robots}
        self.status_times = {robot: deque(maxlen=200) for robot in robots}
        self.service_clients = {}
        for robot in robots:
            self.create_subscription(String, f"/{robot}/body/status", self.body_callback(robot), 10)
            self.create_subscription(String, f"/{robot}/plant/state",
                                     self.json_callback(self.plant, robot), 10)
            self.create_subscription(GoalStatusArray, f"/{robot}/navigate_to_pose/_action/status",
                                     self.action_callback(robot), QoSProfile(
                                         depth=1, durability=DurabilityPolicy.TRANSIENT_LOCAL))

    def action_callback(self, robot):
        def arrived(message):
            for item in message.status_list:
                self.actions[robot][bytes(item.goal_info.goal_id.uuid).hex()] = item.status
        return arrived

    @staticmethod
    def json_callback(target, robot):
        def arrived(message):
            target[robot] = json.loads(message.data)
        return arrived

    def body_callback(self, robot):
        update = self.json_callback(self.body, robot)

        def arrived(message):
            update(message)
            status = self.body[robot]
            self.status_times[robot].append((status["sequence"], status["steadyTimeNs"]))
        return arrived

    def wait(self, condition, description, timeout=8):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            rclpy.spin_once(self, timeout_sec=0.04)
            value = condition()
            if value:
                return value
        raise RuntimeError(f"timeout waiting for {description}")

    def spin_for(self, duration):
        until = time.monotonic() + duration
        while time.monotonic() < until:
            rclpy.spin_once(self, timeout_sec=0.04)

    def call(self, kind, path, request, timeout=8):
        if path not in self.service_clients:
            self.service_clients[path] = self.create_client(kind, path)
        client = self.service_clients[path]
        self.wait(client.service_is_ready, path, timeout)
        future = client.call_async(request)
        self.wait(future.done, path, timeout)
        return future.result()

    def change(self, robot, transition, state):
        request = ChangeState.Request()
        request.transition.id = transition
        if not self.call(ChangeState, f"/{robot}/body_agent/change_state", request).success:
            raise RuntimeError(f"{robot}: lifecycle transition {transition} rejected")
        self.wait(lambda: self.body.get(robot, {}).get("lifecycle") == state,
                  f"{robot} lifecycle {state}")

    def nav2_ready(self, robot):
        return self.call(GetState, f"/{robot}/bt_navigator/get_state", GetState.Request(), 25).current_state.id == 3

    def hold(self, robot, value):
        response = self.call(SetBool, f"/{robot}/plant/hold_motion", SetBool.Request(data=value))
        if not response.success:
            raise RuntimeError("simulation hold failed")

    def reset_plant(self, robot):
        if not self.call(Trigger, f"/{robot}/plant/reset", Trigger.Request()).success:
            raise RuntimeError("simulation reset failed")
        self.wait(lambda: max(abs(self.plant[robot]["x"]), abs(self.plant[robot]["y"])) < 0.02,
                  f"{robot} pose reset")

    def watch_guard(self, robot):
        self.create_subscription(String, f"/{robot}/actuator_sim/status",
                                 self.json_callback(self.actuator_status, robot), 1)

    def verify_qos(self, robot, guarded=False):
        counts = {"reliable": 0, "bestEffort": 0}
        topic = f"/{robot}/scan"

        def receiver(key):
            def arrived(_message):
                counts[key] += 1
            return arrived

        subscribers = [
            self.create_subscription(LaserScan, topic, receiver("reliable"), 1),
            self.create_subscription(LaserScan, topic, receiver("bestEffort"), QoSProfile(
                depth=1, reliability=ReliabilityPolicy.BEST_EFFORT)),
        ]
        try:
            self.wait(lambda: counts["bestEffort"] >= 10, "compatible scan frames")
            offered = self.get_publishers_info_by_topic(topic)
            if (len(offered) != 1 or offered[0].qos_profile.reliability != ReliabilityPolicy.BEST_EFFORT
                    or counts["reliable"] != 0):
                raise RuntimeError("scan QoS isolation did not match the intended offered/requested policy")
            velocity = f"/{robot}/cmd_vel"
            senders = self.get_publishers_info_by_topic(velocity)
            receivers = self.get_subscriptions_info_by_topic(velocity)
            smoothers = [row.qos_profile for row in senders if row.node_name == "velocity_smoother"]
            receiver = "m09_actuator_sim" if guarded else "m08_plant"
            plants = [row.qos_profile for row in receivers if row.node_name == receiver]
            if len(smoothers) != 1 or len(plants) != 1 or any(
                qos.reliability != ReliabilityPolicy.RELIABLE for qos in (smoothers[0], plants[0])
            ):
                raise RuntimeError("velocity publisher and simulator do not have compatible reliable QoS")
            if guarded:
                target = f"/{robot}/actuator_sim/target_until"
                outputs = self.get_publishers_info_by_topic(target)
                inputs = self.get_subscriptions_info_by_topic(target)
                if ([row.node_name for row in outputs] != [receiver] or
                        [row.node_name for row in inputs] != ["m08_plant"] or
                        any(row.node_name == "m08_plant" for row in receivers)):
                    raise RuntimeError("guarded output must be the only route into the simulator")
            return {"topic": topic, "offered": "best_effort", "offeredDepth": offered[0].qos_profile.depth or None,
                    "samplesByRequest": counts, "velocity": {"topic": velocity, "reliability": "reliable",
                        "publisherDepth": smoothers[0].depth or None, "subscriberDepth": plants[0].depth or None}}
        finally:
            for subscriber in subscribers:
                self.destroy_subscription(subscriber)


def callback_isolation():
    def sample(parallel):
        node = Node("m08_callback_probe")
        blocker_group, heartbeat_group = MutuallyExclusiveCallbackGroup(), MutuallyExclusiveCallbackGroup()
        running = threading.Event()
        ended = threading.Event()
        heartbeats = []

        def block():
            blocker.cancel()
            running.set()
            time.sleep(0.55)
            ended.set()

        def heartbeat():
            if running.is_set() and not ended.is_set():
                heartbeats.append(time.monotonic())

        blocker = node.create_timer(0.15, block, callback_group=blocker_group)
        node.create_timer(0.08, heartbeat, callback_group=heartbeat_group)
        executor = MultiThreadedExecutor(num_threads=2) if parallel else SingleThreadedExecutor()
        executor.add_node(node)
        thread = threading.Thread(target=executor.spin)
        thread.start()
        try:
            if not ended.wait(4):
                raise RuntimeError("callback congestion fixture timed out")
            return len(heartbeats)
        finally:
            executor.shutdown(timeout_sec=2)
            thread.join(timeout=2)
            node.destroy_node()

    blocked, isolated = sample(False), sample(True)
    if blocked != 0 or isolated < 3:
        raise RuntimeError("callback isolation did not show the expected improvement")
    return {"blockMs": 550, "singleExecutorHeartbeats": blocked, "separateGroupsHeartbeats": isolated}
