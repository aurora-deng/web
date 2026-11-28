#pragma once
#include "backend/application/modules/ai/application/AiChatService/AiChatService.h"
namespace webserver::phase11::ai_chat_detail {
TimePoint currentTime();
std::string sha256(std::string_view value);
std::pair<std::string,std::uint64_t> promptDigest(const std::vector<PromptMessage>& messages);
void observeLatency(AiChatMetricsSnapshot& metrics,const AiGenerationAudit& audit) noexcept;
}
