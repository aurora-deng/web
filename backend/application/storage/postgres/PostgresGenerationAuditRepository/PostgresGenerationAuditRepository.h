#pragma once
#include "backend/application/domain/GenerationTypes/GenerationTypes.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
#include "backend/application/storage/postgres/PostgresOutboxWriter/PostgresOutboxWriter.h"
namespace webserver::phase11 {
/** GenerationAudit SQL adapter. Uses the caller's connection; never borrows another lease. */
class PostgresGenerationAuditRepository final {
public:
    static bool upsertGenerationAudit(PGconn *connection, AiGenerationAudit audit);
    static std::optional<AiGenerationAudit> findGenerationAudit(PGconn *connection, std::string_view generationId);
};
}
