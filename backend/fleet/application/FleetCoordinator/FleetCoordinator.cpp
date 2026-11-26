#include "FleetCoordinator.h"

#include <algorithm>
#include <utility>

namespace nebula::fleet {
namespace {
std::int64_t reviewTime() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}
}

FleetCoordinator::FleetCoordinator(FleetLedger& ledger, std::chrono::milliseconds heartbeatLimit,
                                   std::function<Clock::time_point()> clock)
    : ledger_(ledger), heartbeatLimit_(heartbeatLimit), clock_(std::move(clock)) {
    if (heartbeatLimit_.count() < 1 || !clock_) throw Error(ErrorCode::invalid, "invalid coordinator clock");
}

Task FleetCoordinator::withConnection(Task task) const {
    if (auto session = connections_.find(task.robotId); session != connections_.end()) {
        task.connected = clock_() - session->second.lastSeen < heartbeatLimit_;
    }
    task.needsReconciliation = task.phase == Phase::uncertain || (active(task.phase) && !task.connected);
    return task;
}

Task FleetCoordinator::submit(std::string_view taskId, Station station, std::string_view traceId) {
    std::lock_guard lock(mutex_);
    return withConnection(ledger_.submit(taskId, station, traceId));
}

Task FleetCoordinator::status(std::string_view taskId) {
    std::lock_guard lock(mutex_);
    auto task = ledger_.lookup(taskId);
    if (!task) throw Error(ErrorCode::missing, "unknown fleet task");
    return withConnection(std::move(*task));
}

std::vector<Task> FleetCoordinator::list(std::string_view cursor,std::size_t limit) {
    std::lock_guard lock(mutex_);
    auto rows=ledger_.list(cursor,limit); for(auto& task:rows) task=withConnection(std::move(task)); return rows;
}

FleetLoad FleetCoordinator::load() {
    std::lock_guard lock(mutex_);
    auto load = ledger_.load();
    const auto now = clock_();
    load.connected = static_cast<unsigned>(std::ranges::count_if(connections_, [&](const auto& entry) {
        return now - entry.second.lastSeen < heartbeatLimit_;
    }));
    return load;
}

SimulationReview FleetCoordinator::stageReview(const ReviewDraft& draft) {
    std::lock_guard lock(mutex_);
    return ledger_.stageReview(draft, reviewTime());
}

SimulationReview FleetCoordinator::inspectReview(std::string_view traceId) {
    std::lock_guard lock(mutex_);
    return ledger_.inspectReview(traceId, reviewTime());
}

SimulationReview FleetCoordinator::decideReview(std::string_view traceId, Decision decision) {
    std::lock_guard lock(mutex_);
    return ledger_.decideReview(traceId, decision, reviewTime());
}

Session FleetCoordinator::connect(std::string robotId, Capabilities capabilities) {
    if (!validId(robotId) || !validCapabilities(capabilities)) throw Error(ErrorCode::invalid, "invalid robot registration");
    std::lock_guard lock(mutex_);
    const auto now = clock_();
    for (auto it = connections_.begin(); it != connections_.end();) {
        if (now - it->second.lastSeen >= heartbeatLimit_) it = connections_.erase(it);
        else ++it;
    }
    if (connections_.contains(robotId)) throw Error(ErrorCode::conflict, "robot already has an active session");
    if (connections_.size() >= 16) throw Error(ErrorCode::exhausted, "too many attached robots");
    const auto generation = ++nextGeneration_;
    connections_.emplace(robotId, Connection{generation, capabilities, now});
    return {std::move(robotId), generation};
}

void FleetCoordinator::disconnect(const Session& session) {
    std::lock_guard lock(mutex_);
    const auto found = connections_.find(session.robotId);
    if (found != connections_.end() && found->second.generation == session.generation) connections_.erase(found);
}

std::optional<Command> FleetCoordinator::exchange(const Session& session, const std::optional<Receipt>& report) {
    std::lock_guard lock(mutex_);
    const auto found = connections_.find(session.robotId);
    const auto now = clock_();
    if (found == connections_.end() || found->second.generation != session.generation ||
        now - found->second.lastSeen >= heartbeatLimit_) {
        throw Error(ErrorCode::offline, "robot session expired");
    }
    found->second.lastSeen = now;
    if (report) ledger_.apply(session.robotId, *report);
    return ledger_.dispatch(session.robotId, found->second.capabilities);
}
}
