#include "BodyLifecycle.h"

#include <algorithm>
#include <lifecycle_msgs/msg/state.hpp>
#include <lifecycle_msgs/msg/transition.hpp>
#include <string>
#include <utility>

namespace nebula::fleet {
namespace {
using namespace std::chrono_literals;
using State = lifecycle_msgs::msg::State;
using Transition = lifecycle_msgs::msg::Transition;
constexpr auto leaseDuration = 3s;
constexpr auto clockFreshness = 1s;

std::uint64_t stampNs(std::chrono::steady_clock::time_point time) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
}
}

BodyLifecycle::BodyLifecycle(const std::string& robotNamespace, const rclcpp::NodeOptions& options)
    : LifecycleNode("body_agent", robotNamespace, options) {}

void BodyLifecycle::start(std::function<bool()> actionReady, bool autostart) {
    actionReady_ = std::move(actionReady);
    transforms_ = std::make_unique<tf2_ros::Buffer>(get_clock());
    transformListener_ = std::make_unique<tf2_ros::TransformListener>(*transforms_, shared_from_this(), false);
    clockSubscription_ = create_subscription<rosgraph_msgs::msg::Clock>(
        "/clock", rclcpp::ClockQoS(), [this](const rosgraph_msgs::msg::Clock& message) {
            const auto stamp = rclcpp::Time(message.clock).nanoseconds();
            if (stamp > 0 && stamp != clockStamp_) lastClockAdvance_.store(Clock::now());
            clockStamp_ = stamp;
        });
    // Report readiness and fault state while the lifecycle node is inactive as well.
    status_ = rclcpp::create_publisher<std_msgs::msg::String>(
        *this, "body/status", rclcpp::QoS(10).reliable().durability_volatile());
    motionPermit_ = rclcpp::create_publisher<std_msgs::msg::UInt64MultiArray>(
        *this, "body/motion_permit", rclcpp::QoS(1).reliable().durability_volatile());
    if (autostart) {
        trigger_transition(Transition::TRANSITION_CONFIGURE);
        resumeWhenHealthy_ = true;
    }
    timer_ = create_wall_timer(100ms, [this] { tick(); });
}

void BodyLifecycle::renewLease() noexcept {
    leaseUntil_.store(Clock::now() + leaseDuration);
}

const char* BodyLifecycle::unavailable(Clock::time_point now) {
    if (uncertain_) return "navigation_unresolved";
    if (now >= leaseUntil_.load()) return "coordinator_lease";
    if (now - lastClockAdvance_.load() >= clockFreshness ||
        !get_clock()->ros_time_is_active() || get_clock()->now().nanoseconds() <= 0) return "simulation_clock";
    try {
        const auto tf = transforms_->lookupTransform("map", "base_link", tf2::TimePointZero);
        const auto stamp = rclcpp::Time(tf.header.stamp, RCL_ROS_TIME);
        const auto age = (get_clock()->now() - stamp).nanoseconds();
        // A static transform is timeless. Dynamic odometry must remain current.
        if (stamp.nanoseconds() != 0 && (age > 1000000000 || age < -1000000000)) return "map_transform";
    } catch (const tf2::TransformException&) {
        return "map_transform";
    }
    if (!actionReady_()) return "navigation_server";
    return nullptr;
}

bool BodyLifecycle::ready() {
    std::lock_guard lock(gate_);
    return enabled_ && !inFlight_ && !unavailable(Clock::now());
}

std::optional<std::uint64_t> BodyLifecycle::dispatch(const Command& command, const std::function<void()>& send) {
    std::lock_guard lock(gate_);
    if (!enabled_ || inFlight_ || unavailable(Clock::now())) return std::nullopt;
    // Order lifecycle revocation and goal dispatch on the same gate.
    taskId_ = command.taskId;
    traceId_ = command.traceId;
    actionId_.clear();
    motionGrant_ = stampNs(Clock::now());
    inFlight_ = true;
    send();
    return generation_;
}

bool BodyLifecycle::permits(std::uint64_t generation) {
    std::lock_guard lock(gate_);
    return enabled_ && generation_ == generation && !unavailable(Clock::now());
}

void BodyLifecycle::accepted(std::string actionId) {
    std::lock_guard lock(gate_);
    actionId_ = std::move(actionId);
}

void BodyLifecycle::complete(bool uncertain) {
    std::lock_guard lock(gate_);
    inFlight_ = false;
    uncertain_ = uncertain_ || uncertain;
}

void BodyLifecycle::revoke() {
    std::lock_guard lock(gate_);
    enabled_ = false;
    ++generation_;
}

BodyLifecycle::Result BodyLifecycle::on_configure(const rclcpp_lifecycle::State&) {
    return get_parameter("use_sim_time").as_bool() ? Result::SUCCESS : Result::FAILURE;
}

BodyLifecycle::Result BodyLifecycle::on_activate(const rclcpp_lifecycle::State&) {
    std::lock_guard lock(gate_);
    if (inFlight_ || unavailable(Clock::now())) return Result::FAILURE;
    enabled_ = true;
    resumeWhenHealthy_ = false;
    return Result::SUCCESS;
}

BodyLifecycle::Result BodyLifecycle::on_deactivate(const rclcpp_lifecycle::State&) {
    revoke();
    return Result::SUCCESS;
}

BodyLifecycle::Result BodyLifecycle::on_cleanup(const rclcpp_lifecycle::State&) {
    std::lock_guard lock(gate_);
    if (inFlight_ || uncertain_) return Result::FAILURE;
    resumeWhenHealthy_ = false;
    return Result::SUCCESS;
}

BodyLifecycle::Result BodyLifecycle::on_shutdown(const rclcpp_lifecycle::State&) {
    revoke();
    return Result::SUCCESS;
}

BodyLifecycle::Result BodyLifecycle::on_error(const rclcpp_lifecycle::State&) {
    revoke();
    resumeWhenHealthy_ = false;
    return Result::SUCCESS;
}

void BodyLifecycle::tick() {
    const auto id = get_current_state().id();
    const auto now = Clock::now();
    if (id == State::PRIMARY_STATE_ACTIVE || (id == State::PRIMARY_STATE_INACTIVE && resumeWhenHealthy_)) {
        bool available, activeGoal;
        {
            std::lock_guard lock(gate_);
            available = unavailable(now) == nullptr;
            activeGoal = inFlight_;
        }
        if (id == State::PRIMARY_STATE_ACTIVE && !available) {
            resumeWhenHealthy_ = true;
            trigger_transition(Transition::TRANSITION_DEACTIVATE);
        } else if (id == State::PRIMARY_STATE_INACTIVE && available && !activeGoal) {
            trigger_transition(Transition::TRANSITION_ACTIVATE);
        }
    }
    const auto leaseUntil = leaseUntil_.load();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(leaseUntil - now).count();
    const auto sampleTime = stampNs(now);
    std_msgs::msg::String status;
    std_msgs::msg::UInt64MultiArray permit;
    {
        std::lock_guard lock(gate_);
        const auto* reason = unavailable(now);
        const char* mode = reason && (resumeWhenHealthy_ || uncertain_) ? "degraded" :
                           enabled_ ? (inFlight_ ? "executing" : "ready") : "standby";
        status.data = std::string(R"({"lifecycle":")") + get_current_state().label() +
                      R"(","mode":")" + mode + R"(","reason":")" + (reason ? reason : "") +
                      R"(","leaseRemainingMs":)" + std::to_string(std::max<std::int64_t>(0, remaining)) +
                      R"(,"taskId":")" + taskId_ + R"(","traceId":")" + traceId_ +
                      R"(","actionId":")" + actionId_ + R"(","feedbackCount":)" +
                      std::to_string(feedbackCount_.load(std::memory_order_relaxed)) +
                      R"(,"sequence":)" + std::to_string(++statusSequence_) +
                      R"(,"steadyTimeNs":)" + std::to_string(sampleTime) +
                      R"(,"inFlight":)" + (inFlight_ ? "true}" : "false}");
        // Simulation-only contract on the same Linux boot: grant, sample time, lease deadline.
        const bool canMove = enabled_ && inFlight_ && !reason && leaseUntil > now && motionGrant_ <= sampleTime;
        permit.data = {canMove ? motionGrant_ : 0, sampleTime, canMove ? stampNs(leaseUntil) : 0};
    }
    motionPermit_->publish(permit);
    status_->publish(status);
}

}
