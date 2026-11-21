#pragma once
#include "backend/application/business/AiChatService/AiChatService.h"
namespace webserver::phase11::ai_chat_detail {
// Executed only on the bounded database executor; callbacks retain the same state.
void prepareGeneration(const std::shared_ptr<AiChatService::State>& state,AiChatRequest request,std::string traceparent) noexcept;
}
