#pragma once
#include "backend/application/modules/ai/domain/KnowledgeCitationView/KnowledgeCitationView.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webserver::phase11 {

class IKnowledgeBusiness
{
public:
    virtual ~IKnowledgeBusiness() = default;

    virtual KnowledgeDocument registerKnowledgeDocument(
        UserId owner, std::optional<ConversationId> conversation,
        std::string title, std::string sourceName,
        std::string mediaType, std::string content, TimePoint now) = 0;
    [[nodiscard]] virtual std::vector<KnowledgeDocument> knowledgeDocuments(
        UserId requester,
        std::optional<ConversationId> conversation) const = 0;
    [[nodiscard]] virtual std::vector<KnowledgeCitationView>
    generationCitations(UserId requester,
                        std::string_view generationId) const = 0;
};

} // namespace webserver::phase11
