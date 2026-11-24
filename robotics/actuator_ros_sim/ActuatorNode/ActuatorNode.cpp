#include "ActuatorRuntime/ActuatorRuntime.h"

#include <geometry_msgs/msg/twist.hpp>
#include <geometry_msgs/msg/twist_stamped.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_msgs/msg/u_int64_multi_array.hpp>
#include <std_srvs/srv/set_bool.hpp>
#include <std_srvs/srv/trigger.hpp>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace nebula::actuator {
namespace {

using namespace std::chrono_literals;

class ActuatorNode final : public rclcpp::Node {
    using Twist = geometry_msgs::msg::Twist;
    using Target = geometry_msgs::msg::TwistStamped;
    using PermitMessage = std_msgs::msg::UInt64MultiArray;
    using Status = std_msgs::msg::String;
    using Emergency = std_srvs::srv::SetBool;
    using Reset = std_srvs::srv::Trigger;

public:
    ActuatorNode() : Node("m09_actuator_sim") {
        const auto qos = rclcpp::QoS(1).reliable().durability_volatile();
        target_ = create_publisher<Target>("actuator_sim/target_until", qos);
        status_ = create_publisher<Status>("actuator_sim/status", qos);
        motion_ = create_subscription<Twist>("cmd_vel", qos, [this](const Twist& message) {
            runtime_.motion(monotonicNow(), {message.linear.x, message.angular.z});
        });
        permit_ = create_subscription<PermitMessage>("body/motion_permit", qos, [this](const PermitMessage& message) {
            const auto& values = message.data;
            constexpr auto bound = static_cast<std::uint64_t>(std::numeric_limits<Time::rep>::max());
            if (values.size() != 3 || values[1] > bound || values[2] > bound || values[0] > values[1]) {
                return;
            }
            const Time sampled(values[1]);
            if (sampled > monotonicNow()) {
                return;
            }
            runtime_.grant({Time(values[0]), sampled, Time(values[2])});
        });
        feedback_ = create_subscription<Target>("actuator_sim/feedback",
            rclcpp::QoS(1).best_effort().durability_volatile(), [this](const Target& message) {
                const auto& stamp = message.header.stamp;
                const auto sampled = std::chrono::seconds(stamp.sec) + Time(stamp.nanosec);
                if (stamp.sec >= 0 && stamp.nanosec < 1'000'000'000 && sampled <= monotonicNow()) {
                    runtime_.feedback({sampled, {message.twist.linear.x, message.twist.angular.z}, true});
                }
            });
        emergency_ = create_service<Emergency>("actuator_sim/emergency",
            [this](const Emergency::Request::SharedPtr request, Emergency::Response::SharedPtr response) {
                runtime_.emergency(request->data);
                response->success = true;
            });
        reset_ = create_service<Reset>("actuator_sim/reset",
            [this](const Reset::Request::SharedPtr, Reset::Response::SharedPtr response) {
                runtime_.requestReset();
                response->success = true;
                response->message = "queued; confirm the result on actuator_sim/status";
            });
        outputTimer_ = create_wall_timer(20ms, [this] { publishTarget(); });
        statusTimer_ = create_wall_timer(100ms, [this] { publishStatus(); });
        runtime_.start();
    }

private:
    void publishTarget() {
        if (const auto sample = runtime_.observe()) {
            last_ = *sample;
        }
        const auto now = monotonicNow();
        const auto& command = last_.output;
        Target message;
        if (command.enabled && command.expires > now) {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(command.expires);
            message.header.stamp.sec = static_cast<std::int32_t>(seconds.count());
            message.header.stamp.nanosec = static_cast<std::uint32_t>((command.expires - seconds).count());
            message.twist.linear.x = command.target.linear;
            message.twist.angular.z = command.target.angular;
        }
        target_->publish(message);
    }

    void publishStatus() {
        Status message;
        const auto* state = last_.state == State::Armed ? "armed" :
                            last_.state == State::Fault ? "fault" : "disarmed";
        message.data = std::string(R"({"state":")") + state + R"(","reason":")" + name(last_.output.reason) +
                       R"(","deadlineMisses":)" + std::to_string(last_.deadlineMisses) +
                       R"(,"skippedSlots":)" + std::to_string(last_.skipped) +
                       R"(,"watchdogTrips":)" + std::to_string(last_.watchdogTrips) +
                       R"(,"wakeLateNs":)" + std::to_string(last_.timing.wakeLateness.count()) +
                       R"(,"executeNs":)" + std::to_string(last_.timing.execution.count()) +
                       R"(,"feedbackAgeNs":)" + std::to_string(last_.feedbackAge.count()) +
                       R"(,"handoffDrops":)" + std::to_string(runtime_.drops()) + "}";
        if (lastReason_ != last_.output.reason || lastTrips_ != last_.watchdogTrips) {
            RCLCPP_INFO(get_logger(), "%s", message.data.c_str());
            lastReason_ = last_.output.reason;
            lastTrips_ = last_.watchdogTrips;
        }
        status_->publish(message);
    }

    ActuatorRuntime runtime_;
    Observation last_{};
    Stop lastReason_{Stop::Disarmed};
    std::uint64_t lastTrips_{};
    rclcpp::Publisher<Target>::SharedPtr target_;
    rclcpp::Publisher<Status>::SharedPtr status_;
    rclcpp::Subscription<Twist>::SharedPtr motion_;
    rclcpp::Subscription<PermitMessage>::SharedPtr permit_;
    rclcpp::Subscription<Target>::SharedPtr feedback_;
    rclcpp::Service<Emergency>::SharedPtr emergency_;
    rclcpp::Service<Reset>::SharedPtr reset_;
    rclcpp::TimerBase::SharedPtr outputTimer_;
    rclcpp::TimerBase::SharedPtr statusTimer_;
};

}  // namespace
}  // namespace nebula::actuator

int main(int argc, char** argv) {
    const auto* loopback = std::getenv("ROS_LOCALHOST_ONLY");
    const auto* domain = std::getenv("ROS_DOMAIN_ID");
    if (!loopback || std::string_view(loopback) != "1" || !domain ||
        std::string_view(domain).empty() || std::string_view(domain).find_first_not_of("0123456789") != std::string_view::npos) {
        std::cerr << "simulation needs ROS_LOCALHOST_ONLY=1 and a dedicated ROS_DOMAIN_ID\n";
        return 2;
    }
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<nebula::actuator::ActuatorNode>());
    rclcpp::shutdown();
}
