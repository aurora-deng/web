#pragma once

#include "backend/fleet/infrastructure/SqliteDatabase/SqliteDatabase.h"
#include "backend/workflow/WorkflowJournal/WorkflowJournal.h"

namespace nebula::systems {

struct JournalLimits {
    std::size_t maxRuns{50000};
    std::uint64_t maxBytes{64 * 1024 * 1024};
};

class SqliteWorkflowJournal final : public workflow::WorkflowJournal {
public:
    explicit SqliteWorkflowJournal(const std::filesystem::path& file, std::filesystem::path snapshot = {},
                                   JournalLimits limits = {});
    std::string open(std::string_view run, std::string_view definition, std::string_view trace) override;
    workflow::Checkpoint node(std::string_view run, std::string_view node) override;
    void start(std::string_view run, std::string_view node, workflow::Effect effect, unsigned attempt) override;
    void settle(std::string_view run, std::string_view node, const workflow::WorkResult& result) override;

private:
    workflow::Checkpoint readNode(std::string_view run, std::string_view node);
    void publish(std::string_view currentRun = {});
    std::filesystem::path snapshot_;
    JournalLimits limits_;
    fleet::sqlite::Database db_;
    std::int64_t pageBytes_{};
    std::int64_t maxPages_{};
    std::mutex access_;
};

}
