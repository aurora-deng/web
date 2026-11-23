#pragma once

#include "backend/fleet/infrastructure/SqliteDatabase/SqliteDatabase.h"
#include "backend/workflow/WorkflowJournal/WorkflowJournal.h"

namespace nebula::systems {

class SqliteWorkflowJournal final : public workflow::WorkflowJournal {
public:
    explicit SqliteWorkflowJournal(const std::filesystem::path& file);
    std::string open(std::string_view run, std::string_view definition, std::string_view trace) override;
    workflow::Checkpoint node(std::string_view run, std::string_view node) override;
    void start(std::string_view run, std::string_view node, workflow::Effect effect, unsigned attempt) override;
    void settle(std::string_view run, std::string_view node, const workflow::WorkResult& result) override;

private:
    workflow::Checkpoint readNode(std::string_view run, std::string_view node);
    fleet::sqlite::Database db_;
    std::mutex access_;
};

}
