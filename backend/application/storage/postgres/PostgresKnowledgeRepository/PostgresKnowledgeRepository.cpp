#include "backend/application/storage/postgres/PostgresKnowledgeRepository/PostgresKnowledgeRepository.h"
#include "backend/application/storage/postgres/PostgresValueCodec/PostgresValueCodec.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace webserver::phase11 {
namespace {
using namespace storage_detail;
std::optional<std::string> encodeOptionalTime(
    const std::optional<TimePoint> &value)
{
    return value ? std::optional<std::string>{encodeTime(*value)}
                 : std::nullopt;
}

std::string knowledgeScope(KnowledgeScope scope)
{
    return scope == KnowledgeScope::PrivateUser ? "private" : "conversation";
}

KnowledgeScope knowledgeScope(std::string_view scope)
{
    if (scope == "private") return KnowledgeScope::PrivateUser;
    if (scope == "conversation") return KnowledgeScope::Conversation;
    throw std::runtime_error("unknown persisted knowledge scope");
}

std::string knowledgeState(KnowledgeDocumentState state)
{
    switch (state)
    {
    case KnowledgeDocumentState::Indexing: return "indexing";
    case KnowledgeDocumentState::Ready: return "ready";
    case KnowledgeDocumentState::Failed: return "failed";
    case KnowledgeDocumentState::Deleted: return "deleted";
    }
    throw std::logic_error("unknown knowledge document state");
}

KnowledgeDocumentState knowledgeState(std::string_view state)
{
    if (state == "indexing") return KnowledgeDocumentState::Indexing;
    if (state == "ready") return KnowledgeDocumentState::Ready;
    if (state == "failed") return KnowledgeDocumentState::Failed;
    if (state == "deleted") return KnowledgeDocumentState::Deleted;
    throw std::runtime_error("unknown persisted knowledge document state");
}

KnowledgeDocument readKnowledgeDocument(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)),
            knowledgeScope(result.value(row, 2)),
            decodeOptionalId(result, row, 3), result.value(row, 4),
            result.value(row, 5), result.value(row, 6), result.value(row, 7),
            knowledgeState(result.value(row, 8)), result.value(row, 9),
            decodeTime(result, row, 10), decodeOptionalTime(result, row, 11)};
}

KnowledgeChunk readKnowledgeChunk(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)),
            std::stoull(result.value(row, 2)), result.value(row, 3),
            result.value(row, 4)};
}

RagRetrieval readRagRetrieval(const Result &result, int row)
{
    return {result.value(row, 0), std::stoull(result.value(row, 1)),
            std::stoull(result.value(row, 2)), result.value(row, 3),
            decodeTime(result, row, 4)};
}

RagCitation readRagCitation(const Result &result, int row)
{
    return {result.value(row, 0), std::stoull(result.value(row, 1)),
            std::stoull(result.value(row, 2)),
            std::stoull(result.value(row, 3)),
            std::stod(result.value(row, 4))};
}

constexpr std::string_view kKnowledgeDocumentColumns =
    "id,owner_id,scope,conversation_id,title,source_name,media_type,sha256,state,"
    "failure_detail,(extract(epoch FROM created_at)*1000000)::bigint,"
    "(extract(epoch FROM indexed_at)*1000000)::bigint";

constexpr std::string_view kKnowledgeChunkColumns =
    "id,document_id,ordinal,content,sha256";

constexpr std::string_view kRagRetrievalColumns =
    "generation_id,requester_id,conversation_id,query_sha256,"
    "(extract(epoch FROM created_at)*1000000)::bigint";

constexpr std::string_view kRagCitationColumns =
    "generation_id,rank,document_id,chunk_id,score::text";
}
KnowledgeDocument PostgresKnowledgeRepository::createKnowledgeDocument(PGconn *connection, bool ownsTransaction, KnowledgeDocument document, std::vector<KnowledgeChunk> chunks)
{
    if (chunks.empty())
        throw std::invalid_argument("knowledge document requires chunks");

    const bool localTransaction = ownsTransaction;
    if (localTransaction) command(connection, "BEGIN");
    try
    {
        const auto inserted = query(connection,
            "INSERT INTO knowledge_documents("
            "owner_id,scope,conversation_id,title,source_name,media_type,sha256,"
            "state,failure_detail,created_at,indexed_at) VALUES("
            "$1::bigint,$2,$3::bigint,$4,$5,$6,$7,$8,$9,"
            "to_timestamp($10::double precision/1000000.0),NULL) RETURNING " +
            std::string(kKnowledgeDocumentColumns),
            {number(document.ownerId), knowledgeScope(document.scope),
             document.conversationId
                 ? std::optional<std::string>{number(*document.conversationId)}
                 : std::nullopt,
             document.title, document.sourceName, document.mediaType,
             document.sha256, knowledgeState(document.state),
             document.failureDetail, encodeTime(document.createdAt)});
        document = readKnowledgeDocument(inserted, 0);
        for (auto &chunk : chunks)
        {
            const auto row = query(connection,
                "INSERT INTO knowledge_chunks(document_id,ordinal,content,sha256) "
                "VALUES($1::bigint,$2::bigint,$3,$4) RETURNING " +
                std::string(kKnowledgeChunkColumns),
                {number(document.id), number(chunk.ordinal), chunk.content,
                 chunk.sha256});
            chunk = readKnowledgeChunk(row, 0);
        }
        if (localTransaction) command(connection, "COMMIT");
        return document;
    }
    catch (...)
    {
        if (localTransaction)
        {
            try { command(connection, "ROLLBACK"); } catch (...) {}
        }
        throw;
    }
}

bool PostgresKnowledgeRepository::updateKnowledgeDocumentState(PGconn *connection, KnowledgeDocumentId document, KnowledgeDocumentState expected, KnowledgeDocumentState next, std::string failureDetail, std::optional<TimePoint> indexedAt)
{

    return query(connection,
        "UPDATE knowledge_documents SET state=$3,failure_detail=$4,indexed_at="
        "CASE WHEN $5::text IS NULL THEN NULL ELSE "
        "to_timestamp($5::double precision/1000000.0) END "
        "WHERE id=$1::bigint AND state=$2 RETURNING 1",
        {number(document), knowledgeState(expected), knowledgeState(next),
         std::move(failureDetail), encodeOptionalTime(indexedAt)}).rows() == 1;
}

std::optional<KnowledgeDocument> PostgresKnowledgeRepository::findKnowledgeDocument(PGconn *connection, KnowledgeDocumentId id)
{

    const auto result = query(connection,
        "SELECT " + std::string(kKnowledgeDocumentColumns) +
        " FROM knowledge_documents WHERE id=$1::bigint", {number(id)});
    return result.rows()
               ? std::optional<KnowledgeDocument>{readKnowledgeDocument(result, 0)}
               : std::nullopt;
}

std::optional<KnowledgeChunk> PostgresKnowledgeRepository::findKnowledgeChunk(PGconn *connection, KnowledgeChunkId id)
{

    const auto result = query(connection,
        "SELECT " + std::string(kKnowledgeChunkColumns) +
        " FROM knowledge_chunks WHERE id=$1::bigint", {number(id)});
    return result.rows()
               ? std::optional<KnowledgeChunk>{readKnowledgeChunk(result, 0)}
               : std::nullopt;
}

std::vector<KnowledgeChunk> PostgresKnowledgeRepository::knowledgeChunks(PGconn *connection, KnowledgeDocumentId document)
{

    const auto result = query(connection,
        "SELECT " + std::string(kKnowledgeChunkColumns) +
        " FROM knowledge_chunks WHERE document_id=$1::bigint ORDER BY ordinal",
        {number(document)});
    std::vector<KnowledgeChunk> chunks;
    chunks.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        chunks.push_back(readKnowledgeChunk(result, row));
    return chunks;
}

std::vector<KnowledgeDocument> PostgresKnowledgeRepository::privateKnowledgeDocuments(PGconn *connection, UserId owner)
{

    const auto result = query(connection,
        "SELECT " + std::string(kKnowledgeDocumentColumns) +
        " FROM knowledge_documents WHERE owner_id=$1::bigint "
        "AND scope='private' AND state='ready' ORDER BY id", {number(owner)});
    std::vector<KnowledgeDocument> documents;
    documents.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        documents.push_back(readKnowledgeDocument(result, row));
    return documents;
}

std::vector<KnowledgeDocument> PostgresKnowledgeRepository::conversationKnowledgeDocuments(PGconn *connection, ConversationId conversation)
{

    const auto result = query(connection,
        "SELECT " + std::string(kKnowledgeDocumentColumns) +
        " FROM knowledge_documents WHERE conversation_id=$1::bigint "
        "AND scope='conversation' AND state='ready' ORDER BY id",
        {number(conversation)});
    std::vector<KnowledgeDocument> documents;
    documents.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        documents.push_back(readKnowledgeDocument(result, row));
    return documents;
}

std::vector<KnowledgeChunk> PostgresKnowledgeRepository::allReadyKnowledgeChunks(PGconn *connection)
{

    const auto result = query(connection,
        "SELECT c.id,c.document_id,c.ordinal,c.content,c.sha256 "
        "FROM knowledge_chunks c JOIN knowledge_documents d ON d.id=c.document_id "
        "WHERE d.state='ready' ORDER BY c.document_id,c.ordinal");
    std::vector<KnowledgeChunk> chunks;
    chunks.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        chunks.push_back(readKnowledgeChunk(result, row));
    return chunks;
}

bool PostgresKnowledgeRepository::saveRagRetrieval(PGconn *connection, bool ownsTransaction, RagRetrieval retrieval, std::vector<RagCitation> citations)
{

    const bool localTransaction = ownsTransaction;
    if (localTransaction) command(connection, "BEGIN");
    try
    {
        const auto inserted = query(connection,
            "INSERT INTO rag_retrievals(generation_id,requester_id,conversation_id,"
            "query_sha256,created_at) VALUES($1,$2::bigint,$3::bigint,$4,"
            "to_timestamp($5::double precision/1000000.0)) "
            "ON CONFLICT(generation_id) DO NOTHING RETURNING 1",
            {retrieval.generationId, number(retrieval.requesterId),
             number(retrieval.conversationId), retrieval.querySha256,
             encodeTime(retrieval.createdAt)});
        if (!inserted.rows())
        {
            if (localTransaction) command(connection, "ROLLBACK");
            return false;
        }
        for (const auto &citation : citations)
            (void)query(connection,
                "INSERT INTO rag_citations(generation_id,rank,document_id,chunk_id,score) "
                "VALUES($1,$2::bigint,$3::bigint,$4::bigint,$5::double precision) "
                "RETURNING 1",
                {retrieval.generationId, number(citation.rank),
                 number(citation.documentId), number(citation.chunkId),
                 std::to_string(citation.score)});
        if (localTransaction) command(connection, "COMMIT");
        return true;
    }
    catch (...)
    {
        if (localTransaction)
        {
            try { command(connection, "ROLLBACK"); } catch (...) {}
        }
        throw;
    }
}

std::optional<RagRetrieval> PostgresKnowledgeRepository::findRagRetrieval(PGconn *connection, std::string_view generationId)
{

    const auto result = query(connection,
        "SELECT " + std::string(kRagRetrievalColumns) +
        " FROM rag_retrievals WHERE generation_id=$1",
        {std::string(generationId)});
    return result.rows()
               ? std::optional<RagRetrieval>{readRagRetrieval(result, 0)}
               : std::nullopt;
}

std::vector<RagCitation> PostgresKnowledgeRepository::ragCitations(PGconn *connection, std::string_view generationId)
{

    const auto result = query(connection,
        "SELECT " + std::string(kRagCitationColumns) +
        " FROM rag_citations WHERE generation_id=$1 ORDER BY rank",
        {std::string(generationId)});
    std::vector<RagCitation> citations;
    citations.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        citations.push_back(readRagCitation(result, row));
    return citations;
}

}
