#pragma once

#include "../../ports/FleetLedger/FleetLedger.h"
#include "../SqliteDatabase/SqliteDatabase.h"

#include <mutex>

namespace nebula::fleet {

class SqliteEdgeLedger final : public EdgeLedger {
public:
    SqliteEdgeLedger(const std::filesystem::path& path, std::string_view robotId);

    void stage(const Command& command) override;
    std::optional<Receipt> pendingReceipt() override;
    void confirm(const Receipt& receipt) override;
    std::optional<Command> beginReady() override;
    void finish(const Command& command, Phase phase, std::string_view outcome) override;

private:
    std::mutex mutex_;
    sqlite::Database db_;
};
}
