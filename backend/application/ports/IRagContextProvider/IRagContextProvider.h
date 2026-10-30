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
class IRagContextProvider
{
public:
    virtual ~IRagContextProvider() = default;
    virtual RagContext retrieveForGeneration(
        std::string generationId, UserId requester,
        ConversationId conversation, std::string_view query,
        std::size_t limit, TimePoint now) = 0;
};
}
