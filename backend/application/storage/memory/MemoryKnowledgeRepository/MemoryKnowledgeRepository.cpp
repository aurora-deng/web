#include "backend/application/storage/memory/MemoryKnowledgeRepository/MemoryKnowledgeRepository.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>
namespace webserver::phase11 {
using storage_detail::orderedUsers;
using storage_detail::UserPair;
using storage_detail::MemberKey;
using storage_detail::IdempotencyKey;
KnowledgeDocument MemoryKnowledgeRepository::createKnowledgeDocument(storage_detail::MemoryState &memory, KnowledgeDocument document, std::vector<KnowledgeChunk> chunks)
{
    const bool validScope =
        (document.scope == KnowledgeScope::PrivateUser &&
         !document.conversationId) ||
        (document.scope == KnowledgeScope::Conversation &&
         document.conversationId &&
         memory.conversations.contains(*document.conversationId));
    if (!memory.users.contains(document.ownerId) || !validScope ||
        document.state != KnowledgeDocumentState::Indexing || chunks.empty())
        throw std::invalid_argument("invalid knowledge document");
    if (document.conversationId &&
        !memory.members.contains({*document.conversationId, document.ownerId}))
        throw std::invalid_argument("knowledge document owner is not a member");

    document.id = memory.nextKnowledgeDocumentId++;
    for (auto &chunk : chunks)
    {
        chunk.id = memory.nextKnowledgeChunkId++;
        chunk.documentId = document.id;
    }
    memory.knowledgeDocuments.emplace(document.id, document);
    for (auto &chunk : chunks)
        memory.knowledgeChunks.emplace(chunk.id, std::move(chunk));
    return document;
}

bool MemoryKnowledgeRepository::updateKnowledgeDocumentState(storage_detail::MemoryState &memory, KnowledgeDocumentId document, KnowledgeDocumentState expected, KnowledgeDocumentState next, std::string failureDetail, std::optional<TimePoint> indexedAt)
{
    const auto found = memory.knowledgeDocuments.find(document);
    if (found == memory.knowledgeDocuments.end() ||
        found->second.state != expected)
        return false;
    found->second.state = next;
    found->second.failureDetail = std::move(failureDetail);
    found->second.indexedAt = indexedAt;
    return true;
}

std::optional<KnowledgeDocument> MemoryKnowledgeRepository::findKnowledgeDocument(const storage_detail::MemoryState &memory, KnowledgeDocumentId id)
{
    const auto found = memory.knowledgeDocuments.find(id);
    return found == memory.knowledgeDocuments.end()
               ? std::nullopt
               : std::optional<KnowledgeDocument>{found->second};
}

std::optional<KnowledgeChunk> MemoryKnowledgeRepository::findKnowledgeChunk(const storage_detail::MemoryState &memory, KnowledgeChunkId id)
{
    const auto found = memory.knowledgeChunks.find(id);
    return found == memory.knowledgeChunks.end()
               ? std::nullopt
               : std::optional<KnowledgeChunk>{found->second};
}

std::vector<KnowledgeChunk> MemoryKnowledgeRepository::knowledgeChunks(const storage_detail::MemoryState &memory, KnowledgeDocumentId document)
{
    std::vector<KnowledgeChunk> result;
    for (const auto &[_, chunk] : memory.knowledgeChunks)
        if (chunk.documentId == document)
            result.push_back(chunk);
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right)
              { return left.ordinal < right.ordinal; });
    return result;
}

std::vector<KnowledgeDocument> MemoryKnowledgeRepository::privateKnowledgeDocuments(const storage_detail::MemoryState &memory, UserId owner)
{
    std::vector<KnowledgeDocument> result;
    for (const auto &[_, document] : memory.knowledgeDocuments)
        if (document.ownerId == owner &&
            document.scope == KnowledgeScope::PrivateUser &&
            document.state == KnowledgeDocumentState::Ready)
            result.push_back(document);
    return result;
}

std::vector<KnowledgeDocument> MemoryKnowledgeRepository::conversationKnowledgeDocuments(const storage_detail::MemoryState &memory, ConversationId conversation)
{
    std::vector<KnowledgeDocument> result;
    for (const auto &[_, document] : memory.knowledgeDocuments)
        if (document.conversationId == conversation &&
            document.scope == KnowledgeScope::Conversation &&
            document.state == KnowledgeDocumentState::Ready)
            result.push_back(document);
    return result;
}

std::vector<KnowledgeChunk> MemoryKnowledgeRepository::allReadyKnowledgeChunks(const storage_detail::MemoryState &memory)
{
    std::vector<KnowledgeChunk> result;
    for (const auto &[_, chunk] : memory.knowledgeChunks)
    {
        const auto document = memory.knowledgeDocuments.find(chunk.documentId);
        if (document != memory.knowledgeDocuments.end() &&
            document->second.state == KnowledgeDocumentState::Ready)
            result.push_back(chunk);
    }
    return result;
}

bool MemoryKnowledgeRepository::saveRagRetrieval(storage_detail::MemoryState &memory, RagRetrieval retrieval, std::vector<RagCitation> citations)
{
    if (retrieval.generationId.empty() ||
        !memory.users.contains(retrieval.requesterId) ||
        !memory.conversations.contains(retrieval.conversationId) ||
        memory.ragRetrievals.contains(retrieval.generationId))
        return false;
    for (std::size_t index = 0; index < citations.size(); ++index)
    {
        const auto &citation = citations[index];
        const auto document = memory.knowledgeDocuments.find(citation.documentId);
        const auto chunk = memory.knowledgeChunks.find(citation.chunkId);
        if (citation.generationId != retrieval.generationId ||
            citation.rank != index + 1 ||
            document == memory.knowledgeDocuments.end() ||
            document->second.state != KnowledgeDocumentState::Ready ||
            chunk == memory.knowledgeChunks.end() ||
            chunk->second.documentId != citation.documentId)
            return false;
    }
    memory.ragRetrievals.emplace(retrieval.generationId, retrieval);
    memory.ragCitations.emplace(retrieval.generationId, std::move(citations));
    return true;
}

std::optional<RagRetrieval> MemoryKnowledgeRepository::findRagRetrieval(const storage_detail::MemoryState &memory, std::string_view generationId)
{
    const auto found = memory.ragRetrievals.find(std::string(generationId));
    return found == memory.ragRetrievals.end()
               ? std::nullopt
               : std::optional<RagRetrieval>{found->second};
}

std::vector<RagCitation> MemoryKnowledgeRepository::ragCitations(const storage_detail::MemoryState &memory, std::string_view generationId)
{
    const auto found = memory.ragCitations.find(std::string(generationId));
    return found == memory.ragCitations.end()
               ? std::vector<RagCitation>{}
               : found->second;
}

}
