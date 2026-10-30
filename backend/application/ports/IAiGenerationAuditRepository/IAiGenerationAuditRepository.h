#pragma once
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include "backend/application/domain/GenerationTypes/GenerationTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace webserver::phase11 {
class IAiGenerationAuditRepository
{
public:
    virtual ~IAiGenerationAuditRepository() = default;
    /** 同一 generationId 只接受 revision 不小于现值的快照。 */
    virtual bool upsertGenerationAudit(AiGenerationAudit audit) = 0;
    [[nodiscard]] virtual std::optional<AiGenerationAudit>
    findGenerationAudit(std::string_view generationId) const = 0;
};
}
