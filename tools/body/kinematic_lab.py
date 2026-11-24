"""Isolated ROS 2 differential-drive simulation for the M08 Nav2 lab."""
import argparse
import json
import math
import os
import time

import rclpy
from geometry_msgs.msg import Quaternion, TransformStamped, Twist, TwistStamped
from nav_msgs.msg import OccupancyGrid, Odometry
from rclpy.executors import SingleThreadedExecutor
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
from rosgraph_msgs.msg import Clock
from sensor_msgs.msg import LaserScan
from std_msgs.msg import String
from std_srvs.srv import SetBool, Trigger
from tf2_ros import StaticTransformBroadcaster, TransformBroadcaster


def orientation(yaw):
    return Quaternion(z=math.sin(yaw / 2), w=math.cos(yaw / 2))


class Simulation:
    def __init__(self, node, origin, publishes_clock):
        self.started = origin if origin is not None else time.monotonic_ns()
        if publishes_clock:
            self.publisher = node.create_publisher(Clock, "/clock", QoSProfile(
                depth=1, reliability=ReliabilityPolicy.BEST_EFFORT))
            self.timer = node.create_timer(0.02, lambda: self.publisher.publish(self.clock()))

    def clock(self):
        message = Clock()
        elapsed = 1_000_000_000 + time.monotonic_ns() - self.started
        message.clock.sec, message.clock.nanosec = divmod(elapsed, 1_000_000_000)
        return message


class Plant(Node):
    def __init__(self, robot, simulation, guarded):
        super().__init__("m08_plant", namespace="/" + robot, use_global_arguments=False,
                         cli_args=["--ros-args", "-r", "/tf:=tf", "-r", "/tf_static:=tf_static"])
        self.simulation = simulation
        self.dynamic_tf = TransformBroadcaster(self)
        self.static_tf = StaticTransformBroadcaster(self)
        map_odom = TransformStamped()
        map_odom.header.frame_id, map_odom.child_frame_id = "map", "odom"
        map_odom.transform.rotation.w = 1.0
        base_laser = TransformStamped()
        base_laser.header.frame_id, base_laser.child_frame_id = "base_link", "laser"
        base_laser.transform.rotation.w = 1.0
        self.static_tf.sendTransform([map_odom, base_laser])

        self.odometry = self.create_publisher(Odometry, "odom", 10)
        self.sensor = self.create_publisher(LaserScan, "scan", QoSProfile(
            depth=1, reliability=ReliabilityPolicy.BEST_EFFORT))
        self.snapshot = self.create_publisher(String, "plant/state", 10)
        self.map = self.create_publisher(OccupancyGrid, "map", QoSProfile(
            depth=1, durability=DurabilityPolicy.TRANSIENT_LOCAL))
        if guarded:
            self.command = self.create_subscription(TwistStamped, "actuator_sim/target_until",
                                                    self.accept_guarded, 1)
            self.feedback_service = self.create_service(SetBool, "plant/feedback", self.set_feedback)
            self.feedback_publisher = self.create_publisher(TwistStamped, "actuator_sim/feedback", QoSProfile(
                depth=1, reliability=ReliabilityPolicy.BEST_EFFORT))
        else:
            self.command = self.create_subscription(Twist, "cmd_vel", self.accept_velocity, 1)
            self.feedback_publisher = None
        self.hold_service = self.create_service(SetBool, "plant/hold_motion", self.hold_motion)
        self.reset_service = self.create_service(Trigger, "plant/reset", self.reset)
        self.x = self.y = self.yaw = 0.0
        self.linear = self.angular = 0.0
        self.command_until = 0
        self.last_update = time.monotonic()
        self.hold = False
        self.feedback_enabled = True
        self.nonzero = 0
        self.create_timer(0.02, self.step)
        self.create_timer(0.1, self.report)
        self.publish_map()

    def accept_velocity(self, message):
        self.set_target(message, time.monotonic_ns() + 300_000_000)

    def accept_guarded(self, message):
        # This simulation topic uses an expiry in the controller's OS clock.
        stamp = message.header.stamp
        now = time.monotonic_ns()
        expiry = stamp.sec * 1_000_000_000 + stamp.nanosec
        if expiry <= now or stamp.nanosec >= 1_000_000_000:
            self.command_until = 0
            return
        self.set_target(message.twist, min(expiry, now + 80_000_000))

    def set_target(self, message, expiry):
        if math.isfinite(message.linear.x) and math.isfinite(message.angular.z):
            self.linear = max(-0.4, min(0.4, message.linear.x))
            self.angular = max(-1.5, min(1.5, message.angular.z))
            self.command_until = expiry
            if abs(self.linear) > 1e-3 or abs(self.angular) > 1e-3:
                self.nonzero += 1
        else:
            self.command_until = 0

    def applied(self):
        if self.hold or time.monotonic_ns() >= self.command_until:
            return 0.0, 0.0
        return self.linear, self.angular

    def set_feedback(self, request, response):
        self.feedback_enabled = request.data
        response.success = True
        return response

    def hold_motion(self, request, response):
        self.hold = request.data
        response.success = True
        return response

    def reset(self, _request, response):
        self.x = self.y = self.yaw = self.linear = self.angular = 0.0
        self.command_until = 0
        self.hold = False
        response.success = True
        return response

    def publish_map(self):
        message = OccupancyGrid()
        message.header.frame_id = "map"
        message.header.stamp = self.simulation.clock().clock
        message.info.resolution = 0.05
        message.info.width = message.info.height = 200
        message.info.origin.position.x = message.info.origin.position.y = -5.0
        message.info.origin.orientation.w = 1.0
        edge = [100] * 200
        middle = [100] + [0] * 198 + [100]
        message.data = edge + middle * 198 + edge
        self.map.publish(message)

    def step(self):
        sampled = time.monotonic_ns()
        now = sampled / 1_000_000_000
        elapsed = min(0.05, now - self.last_update)
        self.last_update = now
        linear, angular = self.applied()
        if self.feedback_publisher and self.feedback_enabled:
            feedback = TwistStamped()
            feedback.header.stamp.sec, feedback.header.stamp.nanosec = divmod(sampled, 1_000_000_000)
            feedback.twist.linear.x, feedback.twist.angular.z = linear, angular
            self.feedback_publisher.publish(feedback)
        self.x += math.cos(self.yaw) * linear * elapsed
        self.y += math.sin(self.yaw) * linear * elapsed
        self.yaw = math.remainder(self.yaw + angular * elapsed, math.tau)
        stamp = self.simulation.clock().clock
        message = Odometry()
        message.header.stamp, message.header.frame_id, message.child_frame_id = stamp, "odom", "base_link"
        pose = message.pose.pose
        pose.position.x, pose.position.y, pose.orientation = self.x, self.y, orientation(self.yaw)
        message.twist.twist.linear.x, message.twist.twist.angular.z = linear, angular
        self.odometry.publish(message)
        transform = TransformStamped()
        transform.header = message.header
        transform.child_frame_id = "base_link"
        transform.transform.translation.x, transform.transform.translation.y = self.x, self.y
        transform.transform.rotation = pose.orientation
        self.dynamic_tf.sendTransform(transform)

    def report(self):
        scan = LaserScan()
        scan.header.stamp, scan.header.frame_id = self.simulation.clock().clock, "laser"
        scan.angle_min, scan.angle_max = -math.pi, math.pi
        scan.angle_increment = math.tau / 72
        scan.range_min, scan.range_max, scan.scan_time = 0.05, 3.5, 0.1
        scan.ranges = [float("inf")] * 73
        self.sensor.publish(scan)
        fresh = time.monotonic_ns() < self.command_until
        linear, angular = self.applied()
        self.snapshot.publish(String(data=json.dumps({
            "x": self.x, "y": self.y, "nonzeroCommands": self.nonzero,
            "commandMoving": fresh and (abs(self.linear) > 1e-3 or abs(self.angular) > 1e-3),
            "linearMps": linear, "angularRadps": angular,
        })))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("robots", nargs="+", choices=("alpha", "beta"))
    parser.add_argument("--guarded", action="store_true", help="consume expiring simulated actuator targets")
    parser.add_argument("--clock-origin-ns", type=int, help="shared monotonic origin for split plant processes")
    parser.add_argument("--passive-clock", action="store_true", help="let the other plant publish the shared clock")
    args = parser.parse_args()
    if len(set(args.robots)) != len(args.robots):
        parser.error("use distinct robot namespaces")
    if args.guarded and (os.environ.get("ROS_LOCALHOST_ONLY") != "1" or not os.environ.get("ROS_DOMAIN_ID", "").isdigit()):
        parser.error("guarded simulation requires loopback and a dedicated ROS_DOMAIN_ID")
    if args.clock_origin_ns is not None and not 0 < args.clock_origin_ns <= time.monotonic_ns():
        parser.error("clock origin must be in the current monotonic clock")
    if args.passive_clock and args.clock_origin_ns is None:
        parser.error("passive clock needs the same origin as the active plant")
    rclpy.init()
    world = Node("m08_clock" if args.clock_origin_ns is None else "m08_clock_" + args.robots[0])
    simulation = Simulation(world, args.clock_origin_ns, not args.passive_clock)
    plants = [Plant(robot, simulation, args.guarded) for robot in args.robots]
    executor = SingleThreadedExecutor()
    for node in [world, *plants]:
        executor.add_node(node)
    try:
        executor.spin()
    except KeyboardInterrupt:
        pass
    finally:
        executor.shutdown()
        for node in [*plants, world]:
            node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == "__main__":
    main()
