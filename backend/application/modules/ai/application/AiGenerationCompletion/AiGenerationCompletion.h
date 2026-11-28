#pragma once
#include "backend/application/modules/ai/application/AiChatService/AiChatService.h"
namespace webserver::phase11::ai_chat_detail {
void publishCompleted(const std::shared_ptr<AiChatService::State>& state,AiCompletedEvent event) noexcept;
void queueAuditSnapshot(const std::shared_ptr<AiChatService::State>& state,AiGenerationAudit audit) noexcept;
void removeAndPublish(const std::shared_ptr<AiChatService::State>& state,const std::string& generationId, bool success,bool cancelled,std::string error,std::optional<Message> message={},AiGenerationStatus failureStatus=AiGenerationStatus::Failed) noexcept;
void watchdogLoop(const std::shared_ptr<AiChatService::State>& state) noexcept;
void onToken(const std::shared_ptr<AiChatService::State>& state,const std::string& generationId,std::string_view token) noexcept;
void onModelCompleted(const std::shared_ptr<AiChatService::State>& state,const std::string& generationId,GenerationCompletion completion) noexcept;
}
