#pragma once
#include "backend/application/modules/shared/domain/IdentityTypes/IdentityTypes.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webserver::phase11 {

struct AiChatRequest
{
    UserId requester = 0;
    UserId aiUser = 0;
    ConversationId conversation = 0;
    std::string requestId;
    std::string prompt;
    std::size_t maxOutputTokens = 512;
    std::string trigger="explicit";
    MessageId replyToMessage=0;
};

} // namespace webserver::phase11
