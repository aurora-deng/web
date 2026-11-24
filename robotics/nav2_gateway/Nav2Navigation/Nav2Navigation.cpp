#include "Nav2Navigation.h"
#include "../BodyLifecycle/BodyLifecycle.h"

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

template<class Future, class Permits>
bool waitFor(Future& future, std::stop_token stop, Clock::time_point deadline, Permits permits) {
    while (!stop.stop_requested() && Clock::now() < deadline && permits()) {
        if (future.wait_until(std::min(deadline, Clock::now() + 20ms)) == std::future_status::ready) return true;
    }
    return false;
}

std::string actionId(const rclcpp_action::GoalUUID& id) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text;
    text.reserve(id.size() * 2);
    for (const auto byte : id) {
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 15]);
    }
    return text;
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
    explicit Impl(const std::string& robotNamespace, bool autostart)
        : context(std::make_shared<rclcpp::Context>()) {
        context->init(0, nullptr);
        rclcpp::NodeOptions nodeOptions;
        nodeOptions.context(context).parameter_overrides({rclcpp::Parameter("use_sim_time", true)})
            .arguments({"--ros-args", "-r", "/tf:=tf", "-r", "/tf_static:=tf_static"});
        node = std::make_shared<BodyLifecycle>(namespaceOf(robotNamespace), nodeOptions);
        actionGroup = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
        client = rclcpp_action::create_client<Action>(node, "navigate_to_pose", actionGroup);
        node->start([this] { return healthy.load() && client->action_server_is_ready(); }, autostart);
        rclcpp::ExecutorOptions executorOptions;
        executorOptions.context = context;
        executor = std::make_unique<rclcpp::executors::MultiThreadedExecutor>(executorOptions, 2);
        executor->add_node(node->get_node_base_interface());
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
    std::shared_ptr<BodyLifecycle> node;
    rclcpp::CallbackGroup::SharedPtr actionGroup;
    Client::SharedPtr client;
    std::unique_ptr<rclcpp::executors::MultiThreadedExecutor> executor;
    std::atomic_bool healthy{true};
    std::jthread spin;
};

Nav2Navigation::Nav2Navigation(std::string robotNamespace, Capabilities stations, bool autostart)
    : stations_(stations) {
    if (!validCapabilities(stations_)) throw Error(ErrorCode::invalid, "invalid Nav2 station grants");
    impl_ = std::make_unique<Impl>(robotNamespace, autostart);
}

Nav2Navigation::~Nav2Navigation() = default;

bool Nav2Navigation::ready() {
    return impl_->context->is_valid() && impl_->node->ready();
}

void Nav2Navigation::renewCoordinatorLease() noexcept {
    impl_->node->renewLease();
}

NavigationResult Nav2Navigation::navigate(const Command& command, std::stop_token stop, Clock::time_point deadline) {
    if (stop.stop_requested() || Clock::now() >= deadline || !hasStation(stations_, command.station) ||
        !validId(command.taskId) || !validTrace(command.traceId) || !ready()) {
        return NavigationResult::rejected;
    }
    Action::Goal message;
    message.pose.header.frame_id = "map";
    message.pose.header.stamp = impl_->node->now();
    message.pose.pose.position.x = command.station == Station::alpha ? 0.5 : 1.0;
    message.pose.pose.position.y = command.station == Station::alpha ? 0.0 : 0.3;
    message.pose.pose.orientation.w = 1.0;

    const auto pending = std::make_shared<Submission>();
    const std::weak_ptr<Client> owner(impl_->client);
    Client::SendGoalOptions options;
    options.goal_response_callback = [pending, owner](Goal handle) {
        cancelGoal(owner, pending->received(std::move(handle)));
    };
    options.feedback_callback = [body = std::weak_ptr<BodyLifecycle>(impl_->node)](
        Goal, std::shared_ptr<const Action::Feedback>) {
        if (const auto node = body.lock()) node->feedback();
    };
    const auto finish = [this](NavigationResult result) {
        impl_->node->complete(result == NavigationResult::uncertain);
        return result;
    };
    try {
        std::shared_future<Goal> accepted;
        const auto generation = impl_->node->dispatch(command, [&] {
            accepted = impl_->client->async_send_goal(message, options);
        });
        if (!generation) return NavigationResult::rejected;
        const auto permits = [&] { return impl_->node->permits(*generation); };
        if (!waitFor(accepted, stop, deadline, permits)) {
            cancelGoal(owner, pending->abandon());
            return finish(NavigationResult::uncertain);
        }
        const auto handle = accepted.get();
        if (!handle) return finish(NavigationResult::rejected);
        impl_->node->accepted(actionId(handle->get_goal_id()));

        auto result = impl_->client->async_get_result(handle);
        if (!waitFor(result, stop, deadline, permits)) {
            cancelGoal(owner, pending->abandon());
            if (!waitFor(result, {}, Clock::now() + 750ms, [] { return true; })) {
                return finish(NavigationResult::uncertain);
            }
        }
        switch (result.get().code) {
        case rclcpp_action::ResultCode::SUCCEEDED: return finish(NavigationResult::arrived);
        case rclcpp_action::ResultCode::CANCELED: return finish(NavigationResult::stopped);
        default: return finish(NavigationResult::uncertain);
        }
    } catch (...) {
        cancelGoal(owner, pending->abandon());
        return finish(NavigationResult::uncertain);
    }
}
}
