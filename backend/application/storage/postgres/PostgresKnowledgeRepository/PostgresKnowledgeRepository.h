#pragma once
#include "backend/application/domain/KnowledgeTypes/KnowledgeTypes.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
#include "backend/application/storage/postgres/PostgresOutboxWriter/PostgresOutboxWriter.h"
namespace webserver::phase11 {
/** Knowledge SQL adapter. Uses the caller's connection; never borrows another lease. */
class PostgresKnowledgeRepository final {
public:
    static KnowledgeDocument createKnowledgeDocument(PGconn *connection, bool ownsTransaction, KnowledgeDocument document, std::vector<KnowledgeChunk> chunks);
    static bool updateKnowledgeDocumentState(PGconn *connection, KnowledgeDocumentId document, KnowledgeDocumentState expected, KnowledgeDocumentState next, std::string failureDetail, std::optional<TimePoint> indexedAt);
    static std::optional<KnowledgeDocument> findKnowledgeDocument(PGconn *connection, KnowledgeDocumentId id);
    static std::optional<KnowledgeChunk> findKnowledgeChunk(PGconn *connection, KnowledgeChunkId id);
    static std::vector<KnowledgeChunk> knowledgeChunks(PGconn *connection, KnowledgeDocumentId document);
    static std::vector<KnowledgeDocument> privateKnowledgeDocuments(PGconn *connection, UserId owner);
    static std::vector<KnowledgeDocument> conversationKnowledgeDocuments(PGconn *connection, ConversationId conversation);
    static std::vector<KnowledgeChunk> allReadyKnowledgeChunks(PGconn *connection);
    static bool saveRagRetrieval(PGconn *connection, bool ownsTransaction, RagRetrieval retrieval, std::vector<RagCitation> citations);
    static std::optional<RagRetrieval> findRagRetrieval(PGconn *connection, std::string_view generationId);
    static std::vector<RagCitation> ragCitations(PGconn *connection, std::string_view generationId);
};
}
