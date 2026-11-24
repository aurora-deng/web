#pragma once

#include "backend/fleet/domain/FleetTypes/FleetTypes.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <rclcpp_lifecycle/lifecycle_node.hpp>
#include <rosgraph_msgs/msg/clock.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_msgs/msg/u_int64_multi_array.hpp>
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>

namespace nebula::fleet {

class BodyLifecycle final : public rclcpp_lifecycle::LifecycleNode {
public:
    BodyLifecycle(const std::string& robotNamespace, const rclcpp::NodeOptions& options);
    void start(std::function<bool()> actionReady, bool autostart);
    void renewLease() noexcept;
    bool ready();
    std::optional<std::uint64_t> dispatch(const Command& command, const std::function<void()>& send);
    bool permits(std::uint64_t generation);
    void accepted(std::string actionId);
    void feedback() noexcept { feedbackCount_.fetch_add(1, std::memory_order_relaxed); }
    void complete(bool uncertain);

private:
    using Clock = std::chrono::steady_clock;
    using Result = rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn;

    Result on_configure(const rclcpp_lifecycle::State&) override;
    Result on_activate(const rclcpp_lifecycle::State&) override;
    Result on_deactivate(const rclcpp_lifecycle::State&) override;
    Result on_cleanup(const rclcpp_lifecycle::State&) override;
    Result on_shutdown(const rclcpp_lifecycle::State&) override;
    Result on_error(const rclcpp_lifecycle::State&) override;

    const char* unavailable(Clock::time_point now);
    void tick();
    void revoke();

    std::mutex gate_;
    std::uint64_t generation_{};
    bool enabled_{};
    bool inFlight_{};
    bool uncertain_{};
    std::uint64_t motionGrant_{};
    std::string taskId_;
    std::string traceId_;
    std::string actionId_;
    std::atomic_uint64_t feedbackCount_{};
    // Lifecycle callbacks and the timer share the default mutually-exclusive group.
    bool resumeWhenHealthy_{};
    std::uint64_t statusSequence_{};
    std::atomic<Clock::time_point> leaseUntil_{};
    std::atomic<Clock::time_point> lastClockAdvance_{};
    std::int64_t clockStamp_{};
    std::function<bool()> actionReady_;
    std::unique_ptr<tf2_ros::Buffer> transforms_;
    std::unique_ptr<tf2_ros::TransformListener> transformListener_;
    rclcpp::Subscription<rosgraph_msgs::msg::Clock>::SharedPtr clockSubscription_;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr status_;
    rclcpp::Publisher<std_msgs::msg::UInt64MultiArray>::SharedPtr motionPermit_;
    rclcpp::TimerBase::SharedPtr timer_;
};

}
