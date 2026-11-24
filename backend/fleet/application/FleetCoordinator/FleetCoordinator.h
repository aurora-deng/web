#pragma once

#include "../../ports/FleetLedger/FleetLedger.h"

#include <chrono>
#include <functional>
#include <map>
#include <mutex>

namespace nebula::fleet {

struct Session {
    std::string robotId;
    std::uint64_t generation;
};

class FleetCoordinator {
public:
    using Clock = std::chrono::steady_clock;
    explicit FleetCoordinator(FleetLedger& ledger, std::chrono::milliseconds heartbeatLimit = std::chrono::milliseconds{800},
                              std::function<Clock::time_point()> clock = Clock::now);

    Task submit(std::string_view taskId, Station station, std::string_view traceId);
    Task status(std::string_view taskId);
    std::vector<Task> listBefore(std::int64_t beforeEpoch, unsigned limit);
    FleetLoad load();
    SimulationReview stageReview(const ReviewDraft& draft);
    SimulationReview inspectReview(std::string_view traceId);
    SimulationReview decideReview(std::string_view traceId, Decision decision);
    Session connect(std::string robotId, Capabilities capabilities);
    void disconnect(const Session& session);
    std::optional<Command> exchange(const Session& session, const std::optional<Receipt>& report);

private:
    struct Connection {
        std::uint64_t generation;
        Capabilities capabilities;
        Clock::time_point lastSeen;
    };

    Task withConnection(Task task) const;

    FleetLedger& ledger_;
    const std::chrono::milliseconds heartbeatLimit_;
    const std::function<Clock::time_point()> clock_;
    std::mutex mutex_;
    std::map<std::string, Connection, std::less<>> connections_;
    std::uint64_t nextGeneration_{};
};
}
