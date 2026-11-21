#pragma once
#include "backend/application/business/AiChatService/AiChatService.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace webserver::phase11 {
// One shared state and one terminal decision survive preparation, callbacks and shutdown.
struct AiChatService::State final
{
    struct ActiveGeneration
    {
        UserId requester = 0;
        UserId aiUser = 0;
        ConversationId conversation = 0;
        CancellationToken cancellation;
        std::vector<UserId> recipients;
        std::string output;
        std::size_t tokenIndex = 0;
        bool completionStarted = false;
        bool outputTooLarge = false;
        std::chrono::steady_clock::time_point deadline{};
        AiGenerationAudit audit;
    };

    State(ChatService &chatService,
          IUserRepository &userRepository,
          IConversationRepository &conversationRepository,
          ModelRouter &modelRouter,
           DatabaseExecutor &executor,
           IAiGenerationAuditRepository &auditRepository,
           IAiGenerationEventSink &eventSink,
           AiChatServiceConfig serviceConfig,
           IRagContextProvider *ragProvider)
        : chat(&chatService), users(&userRepository),
          conversations(&conversationRepository), models(&modelRouter),
          databaseExecutor(&executor), audits(&auditRepository), events(&eventSink),
          rag(ragProvider), config(std::move(serviceConfig))
    {
    }

    ChatService *chat;
    IUserRepository *users;
    IConversationRepository *conversations;
    ModelRouter *models;
    DatabaseExecutor *databaseExecutor;
    IAiGenerationAuditRepository *audits;
    IAiGenerationEventSink *events;
    IRagContextProvider *rag;
    AiChatServiceConfig config;

    std::mutex mutex;
    std::condition_variable changed;
    std::unordered_map<std::string, ActiveGeneration> active;
    AiChatMetricsSnapshot metrics;
    std::thread watchdog;
    bool stopping = false;
};

}
