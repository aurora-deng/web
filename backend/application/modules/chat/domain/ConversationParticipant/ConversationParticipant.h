#pragma once
#include "backend/application/modules/chat/domain/ConversationTypes/ConversationTypes.h"
#include "backend/application/modules/accounts/domain/AccountTypes/AccountTypes.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webserver::phase11 {

struct ConversationParticipant
{
    ConversationMember membership;
    User user;
};

} // namespace webserver::phase11
