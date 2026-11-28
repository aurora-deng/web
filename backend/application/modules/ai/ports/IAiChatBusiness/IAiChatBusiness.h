#pragma once
#include "backend/application/modules/ai/domain/AiChatRequest/AiChatRequest.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webserver::phase11 {

class IAiChatBusiness
{
public:
    virtual ~IAiChatBusiness() = default;
    virtual void generate(AiChatRequest request) = 0;
    [[nodiscard]] virtual bool cancel(UserId requester,
                                      const std::string &generationId) noexcept = 0;
};

} // namespace webserver::phase11
