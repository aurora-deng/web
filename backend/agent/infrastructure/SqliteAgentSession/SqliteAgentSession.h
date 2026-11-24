#pragma once

#include "../../domain/FleetProposal/FleetProposal.h"
#include "backend/fleet/infrastructure/SqliteDatabase/SqliteDatabase.h"

namespace nebula::agent {

class SqliteAgentSession {
public:
    SqliteAgentSession(const std::filesystem::path& file, std::string scope, std::string session);
    std::vector<std::string> context(std::size_t limit);
    std::size_t remember(std::string_view turn, std::span<const TaskEvidence> evidence);

private:
    fleet::sqlite::Database db_;
    std::string scope_;
    std::string session_;
};

}  // namespace nebula::agent
