#include "AiGenerationCompletion.h"
#include "backend/application/modules/ai/application/AiGenerationState/AiGenerationState.h"
#include "backend/application/modules/ai/application/AiGenerationAudit/AiGenerationAudit.h"
#include <algorithm>
#include <stdexcept>
namespace webserver::phase11::ai_chat_detail {
namespace { constexpr std::size_t kMaxPersistedAnswerBytes=16U*1024U; }
void publishCompleted(const std::shared_ptr<AiChatService::State> &state,
                      AiCompletedEvent event) noexcept
{
    try
    {
        state->events->completed(event);
    }
    catch (...)
    {
        // 通知通道故障不能越过模型/数据库 Worker 的线程入口。
    }
}

void queueAuditSnapshot(const std::shared_ptr<AiChatService::State> &state,
                        AiGenerationAudit audit) noexcept
{
    const bool accepted = state->databaseExecutor->submit(
        [repository = state->audits, audit = std::move(audit)]() mutable
        {
            if (!repository->upsertGenerationAudit(std::move(audit)))
                throw std::runtime_error("AI generation audit was rejected");
        });
    std::lock_guard lock(state->mutex);
    if (accepted)
        ++state->metrics.auditSnapshotsQueued;
    else
        ++state->metrics.auditEnqueueRejected;
}

void removeAndPublish(const std::shared_ptr<AiChatService::State> &state,
                      const std::string &generationId,
                      bool success,
                      bool cancelled,
                      std::string error,
                      std::optional<Message> message,
                      AiGenerationStatus failureStatus) noexcept
{
    AiCompletedEvent event;
    AiGenerationAudit audit;
    {
        std::lock_guard lock(state->mutex);
        const auto found = state->active.find(generationId);
        if (found == state->active.end())
            return;
        event.recipients = found->second.recipients;
        event.generationId = generationId;
        event.conversationId = found->second.conversation;
        event.success = success;
        event.cancelled = cancelled;
        event.error = std::move(error);
        event.message = std::move(message);
        auto &active = found->second;
        audit = active.audit;
        audit.outputBytes = active.output.size();
        audit.outputTokens = active.tokenIndex;
        if (!active.output.empty())
        {
            try { audit.outputSha256 = sha256(active.output); }
            catch (...) { audit.outputSha256.clear(); }
        }
        audit.status = success ? AiGenerationStatus::Succeeded
                               : (cancelled ? AiGenerationStatus::Cancelled
                                            : failureStatus);
        audit.errorDetail = event.error;
        if (audit.errorDetail.size() > 512)
            audit.errorDetail.resize(512);
        audit.completedAt = currentTime();
        if (event.message)
            audit.responseMessageId = event.message->id;
        ++audit.revision;
        observeLatency(state->metrics, audit);
        state->active.erase(found);
        if (success)
            ++state->metrics.succeeded;
        else if (cancelled)
            ++state->metrics.cancelled;
        else
            ++state->metrics.failed;
    }
    state->changed.notify_all();
    queueAuditSnapshot(state, std::move(audit));
    publishCompleted(state, std::move(event));
}

void watchdogLoop(const std::shared_ptr<AiChatService::State> &state) noexcept
{
    std::unique_lock lock(state->mutex);
    while (!state->stopping)
    {
        auto nearest = std::chrono::steady_clock::time_point::max();
        for (const auto &[_, generation] : state->active)
            if (!generation.completionStarted)
                nearest = std::min(nearest, generation.deadline);

        if (nearest == std::chrono::steady_clock::time_point::max())
        {
            state->changed.wait(lock);
            continue;
        }
        if (state->changed.wait_until(lock, nearest) != std::cv_status::timeout)
            continue;

        const auto now = std::chrono::steady_clock::now();
        std::vector<CancellationToken> cancellations;
        std::vector<AiCompletedEvent> events;
        std::vector<AiGenerationAudit> audits;
        for (auto current = state->active.begin(); current != state->active.end();)
        {
            auto &generation = current->second;
            if (generation.completionStarted || generation.deadline > now)
            {
                ++current;
                continue;
            }

            generation.completionStarted = true;
            cancellations.push_back(generation.cancellation);
            AiCompletedEvent event;
            event.recipients = generation.recipients;
            event.generationId = current->first;
            event.conversationId = generation.conversation;
            event.error = "AI generation deadline exceeded";
            events.push_back(std::move(event));
            auto audit = generation.audit;
            audit.outputBytes = generation.output.size();
            audit.outputTokens = generation.tokenIndex;
            if (!generation.output.empty())
            {
                try { audit.outputSha256 = sha256(generation.output); }
                catch (...) { audit.outputSha256.clear(); }
            }
            audit.status = AiGenerationStatus::TimedOut;
            audit.errorDetail = "AI generation deadline exceeded";
            audit.completedAt = currentTime();
            ++audit.revision;
            observeLatency(state->metrics, audit);
            audits.push_back(std::move(audit));
            current = state->active.erase(current);
            ++state->metrics.failed;
            ++state->metrics.timedOut;
        }

        lock.unlock();
        for (const auto &cancellation : cancellations)
            cancellation.cancel();
        for (auto &audit : audits)
            queueAuditSnapshot(state, std::move(audit));
        for (auto &event : events)
            publishCompleted(state, std::move(event));
        lock.lock();
    }
}

void onToken(const std::shared_ptr<AiChatService::State> &state,
             const std::string &generationId,
             std::string_view token) noexcept
{
    AiTokenEvent event;
    CancellationToken cancellation;
    bool cancelForLimit = false;
    std::optional<AiGenerationAudit> firstTokenAudit;
    {
        std::lock_guard lock(state->mutex);
        const auto found = state->active.find(generationId);
        if (found == state->active.end() || found->second.completionStarted ||
            state->stopping)
            return;
        auto &active = found->second;
        if (token.size() > kMaxPersistedAnswerBytes - active.output.size())
        {
            active.outputTooLarge = true;
            cancellation = active.cancellation;
            cancelForLimit = true;
        }
        else
        {
            active.output.append(token);
            event.recipients = active.recipients;
            event.generationId = generationId;
            event.conversationId = active.conversation;
            event.tokenIndex = ++active.tokenIndex;
            event.text.assign(token);
            active.audit.outputTokens = active.tokenIndex;
            if (!active.audit.firstTokenAt)
            {
                active.audit.firstTokenAt = currentTime();
                ++active.audit.revision;
                firstTokenAudit = active.audit;
            }
        }
    }

    if (cancelForLimit)
    {
        cancellation.cancel();
        removeAndPublish(state, generationId, false, false,
                         "AI response exceeds the message size limit");
        return;
    }
    if (firstTokenAudit)
        queueAuditSnapshot(state, std::move(*firstTokenAudit));
    if (event.text.empty())
        return;
    try
    {
        state->events->token(event);
    }
    catch (...)
    {
        // SSE 短暂不可用不应取消已经在模型端执行的业务请求。
    }
}

void onModelCompleted(const std::shared_ptr<AiChatService::State> &state,
                      const std::string &generationId,
                      GenerationCompletion completion) noexcept
{
    UserId aiUser = 0;
    ConversationId conversation = 0;
    std::string output;
    bool stopping = false;
    bool outputTooLarge = false;
    {
        std::lock_guard lock(state->mutex);
        const auto found = state->active.find(generationId);
        if (found == state->active.end() || found->second.completionStarted)
            return;
        auto &active = found->second;
        active.completionStarted = true;
        aiUser = active.aiUser;
        conversation = active.conversation;
        output = active.output;
        outputTooLarge = active.outputTooLarge;
        stopping = state->stopping;
        // Once per admitted successful model completion, not per stream piece.
        // Persistence errors remain in separate business outcome metrics.
        if (completion.success && !completion.cancelled && !stopping &&
            !outputTooLarge && !output.empty() && completion.usage)
            state->metrics.inference.observe(*completion.usage);
    }

    if (outputTooLarge)
    {
        removeAndPublish(state, generationId, false, false,
                         "AI response exceeds the message size limit");
        return;
    }
    if (stopping || completion.cancelled)
    {
        removeAndPublish(state, generationId, false, true,
                         completion.error.empty() ? "generation cancelled"
                                                  : std::move(completion.error));
        return;
    }
    if (!completion.success)
    {
        removeAndPublish(state, generationId, false, false,
                         completion.error.empty() ? "model generation failed"
                                                  : std::move(completion.error));
        return;
    }
    if (output.empty())
    {
        removeAndPublish(state, generationId, false, false,
                         "model returned an empty response");
        return;
    }

    const bool accepted = state->databaseExecutor->submit(
        [state, generationId, aiUser, conversation, output = std::move(output),
         trace = std::move(completion.trace)]() mutable
        {
            try
            {
                const auto result = state->chat->sendText(
                    aiUser, conversation, "ai:" + generationId,
                    std::move(output), currentTime(), std::move(trace));
                removeAndPublish(state, generationId, true, false, {},
                                 result.message);
            }
            catch (const std::exception &failure)
            {
                removeAndPublish(state, generationId, false, false,
                                 failure.what());
            }
            catch (...)
            {
                removeAndPublish(state, generationId, false, false,
                                 "AI response persistence failed");
            }
        });
    if (!accepted)
        removeAndPublish(state, generationId, false, false,
                         "database service is busy");
}


}
