#pragma once
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include "backend/application/domain/KnowledgeTypes/KnowledgeTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace webserver::phase11 {
class IKnowledgeIndex
{
public:
    virtual ~IKnowledgeIndex() = default;
    virtual void replaceAll(std::vector<KnowledgeChunk> chunks) = 0;
    virtual void upsert(std::vector<KnowledgeChunk> chunks) = 0;
    virtual void removeDocument(KnowledgeDocumentId document) = 0;
    [[nodiscard]] virtual std::vector<KnowledgeSearchHit> search(
        std::string_view query,
        const std::vector<KnowledgeDocumentId> &allowedDocuments,
        std::size_t limit) const = 0;
};
}
