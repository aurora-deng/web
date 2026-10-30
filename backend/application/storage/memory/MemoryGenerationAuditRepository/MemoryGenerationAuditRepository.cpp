#include "backend/application/storage/memory/MemoryGenerationAuditRepository/MemoryGenerationAuditRepository.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>
namespace webserver::phase11 {
using storage_detail::orderedUsers;
using storage_detail::UserPair;
using storage_detail::MemberKey;
using storage_detail::IdempotencyKey;
bool MemoryGenerationAuditRepository::upsertGenerationAudit(storage_detail::MemoryState &memory, AiGenerationAudit audit)
{
    if (audit.generationId.empty() || audit.promptSha256.size() != 64 ||
        audit.revision == 0)
        return false;
    const auto found = memory.generationAudits.find(audit.generationId);
    if (found != memory.generationAudits.end() &&
        found->second.revision > audit.revision)
        return false;
    memory.generationAudits.insert_or_assign(audit.generationId,
                                             std::move(audit));
    return true;
}

std::optional<AiGenerationAudit> MemoryGenerationAuditRepository::findGenerationAudit(const storage_detail::MemoryState &memory, std::string_view generationId)
{
    const auto found = memory.generationAudits.find(std::string(generationId));
    return found == memory.generationAudits.end()
               ? std::nullopt
               : std::optional<AiGenerationAudit>{found->second};
}

}
