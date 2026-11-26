#include "TaskWorkflow.h"

#include <algorithm>
#include <utility>

namespace nebula::fleet {
using namespace workflow;

TaskWorkflow::TaskWorkflow(NavigationSkill& navigation, Milliseconds navigationTimeout)
    : navigation_(navigation), navigationTimeout_(navigationTimeout) {
    if (navigationTimeout_.count() < 1 || navigationTimeout_.count() > 60000) {
        throw Error(ErrorCode::invalid, "invalid navigation time budget");
    }
}

TaskWorkflow::TaskWorkflow(NavigationSkill& navigation, Milliseconds timeout, WorkflowJournal& journal)
    : TaskWorkflow(navigation, timeout) { durable_ = std::make_unique<DurableWorkflow>(journal, 2); }

Outcome TaskWorkflow::run(const Command& command, std::stop_token stop, std::function<void(const Event&)> sink) {
    const WorkflowGraph graph("fleet.navigation.v1", {
        {"validate", {}, [&](const NodeContext&) {
            checkCommand(command);
            return WorkResult{};
        }},
        {"status", {"validate"}, [&](const NodeContext&) {
            if (!navigation_.ready()) return WorkResult{WorkStatus::failure, {}, "edge_not_ready"};
            return WorkResult{};
        }},
        {"capability", {"validate"}, [&](const NodeContext&) {
            if (!hasStation(navigation_.stations(), command.station)) {
                return WorkResult{WorkStatus::failure, {}, "station_unavailable"};
            }
            return WorkResult{};
        }},
        {"navigate", {"status", "capability"}, [&](const NodeContext& context) {
            auto chassis = navigation_.claimChassis();
            if (!chassis.owns_lock()) return WorkResult{WorkStatus::failure, {}, "chassis_busy"};
            switch (navigation_.navigate(command.station, context.stop, context.deadline)) {
            case NavigationResult::arrived: return WorkResult{};
            case NavigationResult::rejected: return WorkResult{WorkStatus::failure, {}, "navigation_rejected"};
            case NavigationResult::stopped: return WorkResult{WorkStatus::failure, {}, "navigation_stopped"};
            case NavigationResult::uncertain: return WorkResult{WorkStatus::uncertain, {}, "navigation_uncertain"};
            }
            return WorkResult{WorkStatus::uncertain, {}, "navigation_uncertain"};
        }, Effect::external, navigationTimeout_},
        {"report", {"navigate"}, [](const NodeContext&) { return WorkResult{}; }, Effect::idempotent},
    });
    // A queued task may wait for several edge incarnations. Each actual
    // execution gets new event IDs within the same durable task trace.
    RunOptions options{
        .runId = command.taskId,
        .traceId = command.traceId,
        .timeout = navigationTimeout_ + Milliseconds{1000},
        .stop = stop,
        .onEvent = std::move(sink),
    };
    // Identity includes immutable inputs; a changed command cannot reuse old checkpoints.
    const auto revision = "navigation-v2-" + std::to_string(command.epoch) + "-" +
        std::to_string(static_cast<unsigned>(command.station)) + "-" + std::to_string(navigationTimeout_.count());
    RunResult result;
    try {
        result = durable_ ? durable_->run(graph, revision, std::move(options)) : executor_.run(graph, std::move(options));
    } catch (const JournalCapacityExceeded&) {
        // Admission failed before executing any graph node or navigation effect.
        // Report the bounded failure; do not poison the Edge worker or retry motion.
        return {Phase::failed, "workflow_capacity_exhausted"};
    }
    if (result.state == State::success) return {Phase::succeeded, "arrived"};
    const auto navigation = std::ranges::find(result.nodes, "navigate", &NodeResult::nodeId);
    if (navigation->state == State::success ||
        (navigation->attempts && navigation->errorCode != "chassis_busy" && navigation->errorCode != "navigation_rejected" &&
         navigation->errorCode != "navigation_stopped")) {
        return {Phase::uncertain, "navigation_uncertain"};
    }
    if (navigation->state == State::failed) return {Phase::failed, navigation->errorCode};
    return {Phase::failed, result.state == State::cancelled ? "cancelled_before_navigation" : "precondition_failed"};
}
}
