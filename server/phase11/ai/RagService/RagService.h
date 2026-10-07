#pragma once

#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/business/BusinessInterfaces/BusinessInterfaces.h"
#include "server/phase11/ports/Ports/Ports.h"

namespace webserver::phase11
{

struct RagServiceConfig
{
    std::size_t maxDocumentBytes = 2U * 1024U * 1024U;
    std::size_t maxChunkBytes = 1'200;
    std::size_t overlapBytes = 160;
    std::size_t maxRetrievedChunks = 8;
};

/**
 * RAG 应用服务：负责文档授权、确定性切片、索引同步、检索和 Citation 持久化。
 * 它实现 IRagContextProvider，因此 AiChatService 不依赖具体索引设备。
 */
class RagService final : public IRagContextProvider, public IKnowledgeBusiness
{
public:
    RagService(IUserRepository &users,
               IConversationRepository &conversations,
               IKnowledgeRepository &knowledge,
               IKnowledgeIndex &index,
               RagServiceConfig config = {});

    KnowledgeDocument registerKnowledgeDocument(
        UserId owner, std::optional<ConversationId> conversation,
        std::string title, std::string sourceName,
        std::string mediaType, std::string content, TimePoint now) override;

    /** 保留学习代码中的短名称；生产传输层只依赖 IKnowledgeBusiness。 */
    KnowledgeDocument registerDocument(
        UserId owner, std::optional<ConversationId> conversation,
        std::string title, std::string sourceName,
        std::string mediaType, std::string content, TimePoint now)
    {
        return registerKnowledgeDocument(
            owner, conversation, std::move(title), std::move(sourceName),
            std::move(mediaType), std::move(content), now);
    }

    [[nodiscard]] std::vector<KnowledgeDocument> knowledgeDocuments(
        UserId requester,
        std::optional<ConversationId> conversation) const override;

    [[nodiscard]] std::vector<KnowledgeCitationView> generationCitations(
        UserId requester,
        std::string_view generationId) const override;

    RagContext retrieveForGeneration(
        std::string generationId, UserId requester,
        ConversationId conversation, std::string_view query,
        std::size_t limit, TimePoint now) override;

    [[nodiscard]] std::vector<RagCitation> citations(
        UserId requester, std::string_view generationId) const;

private:
    [[nodiscard]] std::vector<KnowledgeChunk> split(
        std::string_view content) const;
    [[nodiscard]] RagContext buildContext(
        const std::vector<RagCitation> &citations) const;
    [[nodiscard]] std::vector<KnowledgeDocumentId> accessibleDocuments(
        UserId requester, ConversationId conversation) const;

    IUserRepository &users_;
    IConversationRepository &conversations_;
    IKnowledgeRepository &knowledge_;
    IKnowledgeIndex &index_;
    RagServiceConfig config_;
};

} // namespace webserver::phase11
