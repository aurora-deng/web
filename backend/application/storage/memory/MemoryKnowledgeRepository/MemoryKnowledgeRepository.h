#pragma once
#include "backend/application/storage/memory/MemoryState/MemoryState.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
namespace webserver::phase11 {
/** Knowledge in-memory adapter. Facade holds the shared lock during calls. */
class MemoryKnowledgeRepository final {
public:
    static KnowledgeDocument createKnowledgeDocument(storage_detail::MemoryState &memory, KnowledgeDocument document, std::vector<KnowledgeChunk> chunks);
    static bool updateKnowledgeDocumentState(storage_detail::MemoryState &memory, KnowledgeDocumentId document, KnowledgeDocumentState expected, KnowledgeDocumentState next, std::string failureDetail, std::optional<TimePoint> indexedAt);
    static std::optional<KnowledgeDocument> findKnowledgeDocument(const storage_detail::MemoryState &memory, KnowledgeDocumentId id);
    static std::optional<KnowledgeChunk> findKnowledgeChunk(const storage_detail::MemoryState &memory, KnowledgeChunkId id);
    static std::vector<KnowledgeChunk> knowledgeChunks(const storage_detail::MemoryState &memory, KnowledgeDocumentId document);
    static std::vector<KnowledgeDocument> privateKnowledgeDocuments(const storage_detail::MemoryState &memory, UserId owner);
    static std::vector<KnowledgeDocument> conversationKnowledgeDocuments(const storage_detail::MemoryState &memory, ConversationId conversation);
    static std::vector<KnowledgeChunk> allReadyKnowledgeChunks(const storage_detail::MemoryState &memory);
    static bool saveRagRetrieval(storage_detail::MemoryState &memory, RagRetrieval retrieval, std::vector<RagCitation> citations);
    static std::optional<RagRetrieval> findRagRetrieval(const storage_detail::MemoryState &memory, std::string_view generationId);
    static std::vector<RagCitation> ragCitations(const storage_detail::MemoryState &memory, std::string_view generationId);
};
}
