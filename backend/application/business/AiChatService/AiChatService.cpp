#include "AiChatService.h"
#include "backend/application/business/AiGenerationState/AiGenerationState.h"
#include "backend/application/business/AiGenerationAudit/AiGenerationAudit.h"
#include "backend/application/business/AiGenerationCompletion/AiGenerationCompletion.h"
#include "backend/application/business/AiGenerationPreparation/AiGenerationPreparation.h"
#include "backend/application/business/ApplicationError/ApplicationError.h"
#include "backend/application/observability/Telemetry/Telemetry.h"
#include <algorithm>
namespace webserver::phase11 {
using namespace ai_chat_detail;
AiChatService::AiChatService(ChatService &chat,
                             IUserRepository &users,
                             IConversationRepository &conversations,
                             ModelRouter &models,
                             DatabaseExecutor &databaseExecutor,
                             IAiGenerationAuditRepository &audits,
                             IAiGenerationEventSink &events,
                             AiChatServiceConfig config,
                             IRagContextProvider *rag)
    : state_(std::make_shared<State>(chat, users, conversations, models,
                                      databaseExecutor, audits, events,
                                      std::move(config), rag))
{
    if (state_->config.maxActiveGenerations == 0 ||
        state_->config.maxActivePerUser == 0 ||
        state_->config.maxActivePerAiAccount == 0 ||
        state_->config.ragMaxChunks == 0 || state_->config.ragMaxChunks > 32 ||
        state_->config.generationTimeout <= std::chrono::milliseconds::zero())
        throw std::invalid_argument("invalid AI chat service configuration");
    state_->watchdog = std::thread([state = state_] { watchdogLoop(state); });
}

AiChatService::~AiChatService()
{
    shutdown();
}

void AiChatService::generate(AiChatRequest request)
{
    if (request.requester == 0 || request.aiUser == 0 ||
        request.conversation == 0 || request.requestId.empty() ||
        request.requestId.size() > 125 || request.prompt.empty() ||
        request.prompt.size() > 16U * 1024U || request.maxOutputTokens == 0 ||
        request.maxOutputTokens > 4096)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid AI generation request");

    auto state = state_;
    {
        std::lock_guard lock(state->mutex);
        if (state->stopping)
            throw ApplicationError(ErrorCode::Unavailable,
                                   "AI chat service is stopping");
        const auto userActive = static_cast<std::size_t>(std::count_if(
            state->active.begin(), state->active.end(),
            [&](const auto &entry)
            { return entry.second.requester == request.requester; }));
        const auto aiActive = static_cast<std::size_t>(std::count_if(
            state->active.begin(), state->active.end(),
            [&](const auto &entry)
            { return entry.second.aiUser == request.aiUser; }));
        if (state->active.size() >= state->config.maxActiveGenerations ||
            userActive >= state->config.maxActivePerUser ||
            aiActive >= state->config.maxActivePerAiAccount)
        {
            ++state->metrics.quotaRejected;
            throw ApplicationError(ErrorCode::RateLimited,
                                   "too many active AI generations");
        }
        State::ActiveGeneration active;
        active.requester = request.requester;
        active.aiUser = request.aiUser;
        active.conversation = request.conversation;
        // 深层异步校验失败时也必须能通知发起者；校验通过后再扩展为会话成员快照。
        active.recipients.push_back(request.requester);
        active.deadline = std::chrono::steady_clock::now() +
                          state->config.generationTimeout;
        active.audit.generationId = request.requestId;
        active.audit.requesterId = request.requester;
        active.audit.aiUserId = request.aiUser;
        active.audit.conversationId = request.conversation;
        active.audit.promptSha256 = sha256(request.prompt);
        active.audit.promptBytes = request.prompt.size();
        active.audit.status = AiGenerationStatus::Queued;
        active.audit.requestedAt = currentTime();
        if (!state->active.emplace(request.requestId, std::move(active)).second)
            throw ApplicationError(ErrorCode::Conflict,
                                   "generation id is already active");
        ++state->metrics.accepted;
    }
    state->changed.notify_all();

    const auto generationId = request.requestId;
    if (!state->databaseExecutor->submit(
            [state, request = std::move(request),parent=telemetry::Span::traceparent()]() mutable
            {
                prepareGeneration(state, std::move(request),std::move(parent));
            }))
    {
        removeAndPublish(state, generationId, false, false,
                         "database service is busy");
        throw ApplicationError(ErrorCode::Unavailable,
                               "database service is busy");
    }
}

bool AiChatService::cancel(UserId requester,
                           const std::string &generationId) noexcept
{
    CancellationToken cancellation;
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->active.find(generationId);
        if (found == state_->active.end() ||
            found->second.requester != requester ||
            found->second.completionStarted)
            return false;
        cancellation = found->second.cancellation;
    }
    cancellation.cancel();
    removeAndPublish(state_, generationId, false, true,
                     "generation cancelled");
    return true;
}

AiChatMetricsSnapshot AiChatService::snapshot() const noexcept
{
    AiChatMetricsSnapshot result;
    const auto state = state_;
    if (!state)
        return result;
    std::lock_guard lock(state->mutex);
    result = state->metrics;
    result.active = state->active.size();
    return result;
}

void AiChatService::shutdown() noexcept
{
    auto state = state_;
    if (!state)
        return;
    std::vector<CancellationToken> cancellations;
    std::vector<AiCompletedEvent> events;
    std::vector<AiGenerationAudit> audits;
    {
        std::lock_guard lock(state->mutex);
        state->stopping = true;
        cancellations.reserve(state->active.size());
        events.reserve(state->active.size());
        for (auto current = state->active.begin();
             current != state->active.end();)
        {
            const auto &id = current->first;
            const auto &generation = current->second;
            // 模型完成后，AI 回复可能已经进入数据库执行器。这个阶段取消输给完成，
            // shutdown 在下面等待它持久化，避免先通知 cancelled 后又写入成功消息。
            if (generation.completionStarted)
            {
                ++current;
                continue;
            }
            cancellations.push_back(generation.cancellation);
            AiCompletedEvent event;
            event.recipients = generation.recipients;
            event.generationId = id;
            event.conversationId = generation.conversation;
            event.cancelled = true;
            event.error = "AI chat service is stopping";
            events.push_back(std::move(event));
            auto audit = generation.audit;
            audit.outputBytes = generation.output.size();
            audit.outputTokens = generation.tokenIndex;
            if (!generation.output.empty())
            {
                try { audit.outputSha256 = sha256(generation.output); }
                catch (...) { audit.outputSha256.clear(); }
            }
            audit.status = AiGenerationStatus::Cancelled;
            audit.errorDetail = "AI chat service is stopping";
            audit.completedAt = currentTime();
            ++audit.revision;
            observeLatency(state->metrics, audit);
            audits.push_back(std::move(audit));
            current = state->active.erase(current);
            ++state->metrics.cancelled;
        }
    }
    state->changed.notify_all();
    for (const auto &cancellation : cancellations)
        cancellation.cancel();
    for (auto &audit : audits)
        queueAuditSnapshot(state, std::move(audit));
    for (auto &event : events)
        publishCompleted(state, std::move(event));
    if (state->watchdog.joinable())
        state->watchdog.join();

    // 这里只等待已经赢得完成竞态、正在落库的请求；仍在模型端的请求已在上面
    // 强制摘除，所以一个失联 Provider 不会再拖住关机。
    std::unique_lock lock(state->mutex);
    state->changed.wait(lock, [&] { return state->active.empty(); });
}

} // namespace webserver::phase11
