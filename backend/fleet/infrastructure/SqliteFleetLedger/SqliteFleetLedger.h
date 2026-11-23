#pragma once

#include "../../ports/FleetLedger/FleetLedger.h"
#include "../SqliteDatabase/SqliteDatabase.h"

namespace nebula::fleet {

class SqliteFleetLedger final : public FleetLedger {
public:
    explicit SqliteFleetLedger(const std::filesystem::path& path, std::size_t queueLimit = 256);

    Task submit(std::string_view taskId, Station station, std::string_view traceId) override;
    std::optional<Task> lookup(std::string_view taskId) override;
    std::optional<Command> dispatch(std::string_view robotId, Capabilities capabilities) override;
    void apply(std::string_view robotId, const Receipt& receipt) override;
    FleetLoad load() override;
    SimulationReview stageReview(const ReviewDraft& draft, std::int64_t now) override;
    SimulationReview inspectReview(std::string_view traceId, std::int64_t now) override;
    SimulationReview decideReview(std::string_view traceId, Decision decision, std::int64_t now) override;

private:
    void createReviewTable();
    std::optional<SimulationReview> lookupReview(std::string_view traceId);
    Task insertTask(std::string_view taskId, Station station, std::string_view traceId);
    sqlite::Database db_;
    std::size_t queueLimit_;
};
}
