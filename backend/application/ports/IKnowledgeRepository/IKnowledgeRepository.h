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
class IKnowledgeRepository
{
public:
    virtual ~IKnowledgeRepository() = default;
    /** 文档登记卡与全部 Chunk 必须原子写入，不能留下半份知识。 */
    virtual KnowledgeDocument createKnowledgeDocument(
        KnowledgeDocument document, std::vector<KnowledgeChunk> chunks) = 0;
    virtual bool updateKnowledgeDocumentState(
        KnowledgeDocumentId document, KnowledgeDocumentState expected,
        KnowledgeDocumentState next, std::string failureDetail,
        std::optional<TimePoint> indexedAt) = 0;
    [[nodiscard]] virtual std::optional<KnowledgeDocument>
    findKnowledgeDocument(KnowledgeDocumentId id) const = 0;
    [[nodiscard]] virtual std::optional<KnowledgeChunk>
    findKnowledgeChunk(KnowledgeChunkId id) const = 0;
    [[nodiscard]] virtual std::vector<KnowledgeChunk> knowledgeChunks(
        KnowledgeDocumentId document) const = 0;
    [[nodiscard]] virtual std::vector<KnowledgeDocument>
    privateKnowledgeDocuments(UserId owner) const = 0;
    [[nodiscard]] virtual std::vector<KnowledgeDocument>
    conversationKnowledgeDocuments(ConversationId conversation) const = 0;
    [[nodiscard]] virtual std::vector<KnowledgeChunk>
    allReadyKnowledgeChunks() const = 0;
    /** Retrieval 与有序 Citation 一起保存，generationId 只允许写入一次。 */
    virtual bool saveRagRetrieval(RagRetrieval retrieval,
                                  std::vector<RagCitation> citations) = 0;
    [[nodiscard]] virtual std::optional<RagRetrieval> findRagRetrieval(
        std::string_view generationId) const = 0;
    [[nodiscard]] virtual std::vector<RagCitation> ragCitations(
        std::string_view generationId) const = 0;
};
}
