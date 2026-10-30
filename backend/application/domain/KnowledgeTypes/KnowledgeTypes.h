#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include "backend/application/domain/ConversationTypes/ConversationTypes.h"

namespace webserver::phase11 {
/** KnowledgeTypes: protocol/database independent business facts. */
enum class KnowledgeScope
{
    PrivateUser,
    Conversation
};

enum class KnowledgeDocumentState
{
    Indexing,
    Ready,
    Failed,
    Deleted
};

struct KnowledgeDocument
{
    KnowledgeDocumentId id = 0;
    UserId ownerId = 0;
    KnowledgeScope scope = KnowledgeScope::PrivateUser;
    std::optional<ConversationId> conversationId;
    std::string title;
    std::string sourceName;
    std::string mediaType;
    std::string sha256;
    KnowledgeDocumentState state = KnowledgeDocumentState::Indexing;
    std::string failureDetail;
    TimePoint createdAt{};
    std::optional<TimePoint> indexedAt;
};

struct KnowledgeChunk
{
    KnowledgeChunkId id = 0;
    KnowledgeDocumentId documentId = 0;
    std::uint64_t ordinal = 0;
    std::string content;
    std::string sha256;
};

struct KnowledgeSearchHit
{
    KnowledgeDocumentId documentId = 0;
    KnowledgeChunkId chunkId = 0;
    double score = 0.0;
};

struct RagRetrieval
{
    std::string generationId;
    UserId requesterId = 0;
    ConversationId conversationId = 0;
    std::string querySha256;
    TimePoint createdAt{};
};

struct RagCitation
{
    std::string generationId;
    std::uint64_t rank = 0;
    KnowledgeDocumentId documentId = 0;
    KnowledgeChunkId chunkId = 0;
    double score = 0.0;

    bool operator==(const RagCitation &) const = default;
};

struct RagContext
{
    std::string systemPrompt;
    std::vector<RagCitation> citations;
};
}
