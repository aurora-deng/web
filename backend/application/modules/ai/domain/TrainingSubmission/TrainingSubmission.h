#pragma once
#include "backend/application/modules/shared/domain/IdentityTypes/IdentityTypes.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webserver::phase11 {

struct TrainingSubmission
{
    UserId ownerUserId = 0;
    MessageId sourceMessageId = 0;
    std::string sanitizedPrompt;
    std::string sanitizedResponse;
    std::string consentVersion;
    bool consentGranted = false;
};

} // namespace webserver::phase11
