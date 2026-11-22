#include "robotics/nav2_gateway/Nav2Navigation/Nav2Navigation.h"

#include <atomic>
#include <iostream>
#include <nav2_msgs/action/navigate_to_pose.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <rosgraph_msgs/msg/clock.hpp>
#include <thread>

using namespace nebula::fleet;
using namespace std::chrono_literals;

namespace {
using Action = nav2_msgs::action::NavigateToPose;
using GoalHandle = rclcpp_action::ServerGoalHandle<Action>;
using Clock = std::chrono::steady_clock;

class SimAction {
public:
    explicit SimAction(std::string robotNamespace = "/nebula_lab")
        : node_(std::make_shared<rclcpp::Node>("action_fixture", robotNamespace)) {
        clock_ = node_->create_publisher<rosgraph_msgs::msg::Clock>("/clock", rclcpp::ClockQoS());
        server_ = rclcpp_action::create_server<Action>(
            node_, "navigate_to_pose",
            [&](const rclcpp_action::GoalUUID&, std::shared_ptr<const Action::Goal> goal) {
                if (goal->pose.header.frame_id != "map" || goal->pose.pose.position.x != 0.5) {
                    return rclcpp_action::GoalResponse::REJECT;
                }
                return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
            },
            [](const std::shared_ptr<GoalHandle>&) { return rclcpp_action::CancelResponse::ACCEPT; },
            [&](std::shared_ptr<GoalHandle> goal) {
                active_ = std::move(goal);
                received.fetch_add(1);
            });
        tick_ = node_->create_wall_timer(20ms, [&] { tick(); });
        executor_.add_node(node_);
        spin_ = std::jthread([&] { executor_.spin(); });
    }

    ~SimAction() {
        if (rclcpp::ok()) rclcpp::shutdown();
        executor_.cancel();
        spin_.join();
    }

    std::atomic_bool hold{false};
    std::atomic_uint received{0};
    std::atomic_uint cancelled{0};

private:
    void tick() {
        rosgraph_msgs::msg::Clock message;
        message.clock.sec = ++seconds_;
        clock_->publish(message);
        if (!active_) return;
        if (active_->is_canceling()) {
            ++cancelled;
            active_->canceled(std::make_shared<Action::Result>());
            active_.reset();
        } else if (!hold.load()) {
            active_->succeed(std::make_shared<Action::Result>());
            active_.reset();
        }
    }

    const rclcpp::Node::SharedPtr node_;
    rclcpp::Publisher<rosgraph_msgs::msg::Clock>::SharedPtr clock_;
    rclcpp_action::Server<Action>::SharedPtr server_;
    rclcpp::TimerBase::SharedPtr tick_;
    std::shared_ptr<GoalHandle> active_;
    int seconds_{0};
    rclcpp::executors::SingleThreadedExecutor executor_;
    std::jthread spin_;
};

void require(bool passed, const char* issue) {
    if (!passed) throw std::runtime_error(issue);
}

void contract() {
    SimAction sim;
    Nav2Navigation navigation("nebula_lab", 1);
    const auto limit = Clock::now() + 6s;
    while (!navigation.ready() && Clock::now() < limit) std::this_thread::sleep_for(10ms);
    require(navigation.ready(), "ROS discovery or simulated clock did not become ready");
    require(navigation.navigate(Station::alpha, {}, Clock::now() + 3s) == NavigationResult::arrived,
            "NavigateToPose did not return a success result");
    require(navigation.navigate(Station::beta, {}, Clock::now() + 3s) == NavigationResult::rejected &&
            sim.received.load() == 1, "unsupported station reached the ROS action server");

    sim.hold.store(true);
    std::stop_source stop;
    std::jthread cancel([&] {
        const auto deadline = Clock::now() + 2s;
        while (sim.received.load() < 2 && Clock::now() < deadline) std::this_thread::sleep_for(5ms);
        stop.request_stop();
    });
    const auto result = navigation.navigate(Station::alpha, stop.get_token(), Clock::now() + 3s);
    require(result == NavigationResult::stopped && sim.cancelled.load() == 1,
            "cancel did not finish the same ROS action");
}
}

int main(int argc, char* argv[]) {
    rclcpp::init(0, nullptr);
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--serve-alpha") {
            SimAction fixture("/alpha");
            std::cout << "SIM_ACTION_READY" << std::endl;
            while (rclcpp::ok()) std::this_thread::sleep_for(50ms);
        } else if (argc == 1) {
            contract();
            std::cout << "ROS action contract passed: simulated clock, arrival, local grant and cancellation\n";
        } else {
            throw std::invalid_argument("usage: nebula_nav2_contract [--serve-alpha]");
        }
        rclcpp::shutdown();
    } catch (const std::exception& error) {
        rclcpp::shutdown();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
