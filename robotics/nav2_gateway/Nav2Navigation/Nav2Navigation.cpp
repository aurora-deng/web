#include "Nav2Navigation.h"

#include <algorithm>
#include <atomic>
#include <future>
#include <mutex>
#include <nav2_msgs/action/navigate_to_pose.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <thread>

namespace nebula::fleet {
namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Action = nav2_msgs::action::NavigateToPose;
using Client = rclcpp_action::Client<Action>;
using Goal = Client::GoalHandle::SharedPtr;

std::string namespaceOf(const std::string& id) {
    if (!validId(id) || id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") !=
                            std::string::npos || (id.front() >= '0' && id.front() <= '9')) {
        throw Error(ErrorCode::invalid, "invalid Nav2 robot namespace");
    }
    return "/" + id;
}

template<class Future>
bool waitFor(Future& future, std::stop_token stop, Clock::time_point deadline) {
    while (!stop.stop_requested() && Clock::now() < deadline) {
        if (future.wait_until(std::min(deadline, Clock::now() + 20ms)) == std::future_status::ready) return true;
    }
    return false;
}

void cancelGoal(const std::weak_ptr<Client>& owner, const Goal& goal) noexcept {
    if (!goal) return;
    try {
        if (auto client = owner.lock()) client->async_cancel_goal(goal);
    } catch (...) {
        // A vanished handle cannot prove the result; the ledger retains uncertainty.
    }
}

struct Submission {
    std::mutex mutex;
    bool abandoned{};
    Goal handle;

    Goal received(Goal value) {
        std::lock_guard lock(mutex);
        handle = std::move(value);
        return abandoned ? handle : nullptr;
    }

    Goal abandon() {
        std::lock_guard lock(mutex);
        abandoned = true;
        return handle;
    }
};
}

struct Nav2Navigation::Impl {
    explicit Impl(const std::string& robotNamespace)
        : context(std::make_shared<rclcpp::Context>()) {
        context->init(0, nullptr);
        rclcpp::NodeOptions nodeOptions;
        nodeOptions.context(context).parameter_overrides({rclcpp::Parameter("use_sim_time", true)});
        node = std::make_shared<rclcpp::Node>("nebula_fleet_edge", namespaceOf(robotNamespace), nodeOptions);
        client = rclcpp_action::create_client<Action>(node, "navigate_to_pose");
        rclcpp::ExecutorOptions executorOptions;
        executorOptions.context = context;
        executor = std::make_unique<rclcpp::executors::SingleThreadedExecutor>(executorOptions);
        executor->add_node(node);
        spin = std::jthread([this] {
            try { executor->spin(); }
            catch (...) { healthy.store(false); }
        });
    }

    ~Impl() {
        try { context->shutdown("NEBULA simulation edge stopped"); }
        catch (...) {}
        if (spin.joinable()) spin.join();
    }

    rclcpp::Context::SharedPtr context;
    rclcpp::Node::SharedPtr node;
    Client::SharedPtr client;
    std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor;
    std::atomic_bool healthy{true};
    std::jthread spin;
};

Nav2Navigation::Nav2Navigation(std::string robotNamespace, Capabilities stations)
    : stations_(stations), impl_(std::make_unique<Impl>(robotNamespace)) {
    if (!validCapabilities(stations_)) throw Error(ErrorCode::invalid, "invalid Nav2 station grants");
}

Nav2Navigation::~Nav2Navigation() = default;

bool Nav2Navigation::ready() {
    return impl_->healthy.load() && impl_->context->is_valid() &&
           impl_->node->get_parameter("use_sim_time").as_bool() &&
           impl_->node->get_clock()->ros_time_is_active() &&
           impl_->node->get_clock()->now().nanoseconds() > 0 && impl_->client->action_server_is_ready();
}

NavigationResult Nav2Navigation::navigate(Station station, std::stop_token stop, Clock::time_point deadline) {
    if (stop.stop_requested() || Clock::now() >= deadline || !hasStation(stations_, station) || !ready()) {
        return NavigationResult::rejected;
    }
    Action::Goal message;
    message.pose.header.frame_id = "map";
    message.pose.header.stamp = impl_->node->now();
    message.pose.pose.position.x = station == Station::alpha ? 0.5 : 1.0;
    message.pose.pose.position.y = station == Station::alpha ? 0.0 : 0.3;
    message.pose.pose.orientation.w = 1.0;

    const auto pending = std::make_shared<Submission>();
    const std::weak_ptr<Client> owner(impl_->client);
    Client::SendGoalOptions options;
    options.goal_response_callback = [pending, owner](Goal handle) {
        cancelGoal(owner, pending->received(std::move(handle)));
    };
    try {
        auto accepted = impl_->client->async_send_goal(message, options);
        if (!waitFor(accepted, stop, deadline)) {
            cancelGoal(owner, pending->abandon());
            return NavigationResult::uncertain;
        }
        const auto handle = accepted.get();
        if (!handle) return NavigationResult::rejected;

        auto result = impl_->client->async_get_result(handle);
        if (!waitFor(result, stop, deadline)) {
            cancelGoal(owner, pending->abandon());
            if (!waitFor(result, {}, Clock::now() + 750ms)) return NavigationResult::uncertain;
        }
        switch (result.get().code) {
        case rclcpp_action::ResultCode::SUCCEEDED: return NavigationResult::arrived;
        case rclcpp_action::ResultCode::CANCELED: return NavigationResult::stopped;
        default: return NavigationResult::uncertain;
        }
    } catch (...) {
        cancelGoal(owner, pending->abandon());
        return NavigationResult::uncertain;
    }
}
}
