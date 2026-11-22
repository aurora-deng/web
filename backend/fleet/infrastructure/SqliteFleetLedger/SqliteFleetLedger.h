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

private:
    sqlite::Database db_;
    std::size_t queueLimit_;
};
}
