#pragma once
#include "backend/application/storage/memory/MemoryState/MemoryState.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
namespace webserver::phase11 {
/** GenerationAudit in-memory adapter. Facade holds the shared lock during calls. */
class MemoryGenerationAuditRepository final {
public:
    static bool upsertGenerationAudit(storage_detail::MemoryState &memory, AiGenerationAudit audit);
    static std::optional<AiGenerationAudit> findGenerationAudit(const storage_detail::MemoryState &memory, std::string_view generationId);
};
}
