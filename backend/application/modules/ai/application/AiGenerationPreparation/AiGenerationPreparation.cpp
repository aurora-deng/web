#include "AiGenerationPreparation.h"
#include "backend/application/modules/ai/application/AiGenerationState/AiGenerationState.h"
#include "backend/application/modules/ai/application/AiGenerationAudit/AiGenerationAudit.h"
#include "backend/application/modules/ai/application/AiGenerationCompletion/AiGenerationCompletion.h"
#include "backend/application/modules/shared/domain/ApplicationError/ApplicationError.h"
#include "backend/application/modules/assistant/application/AiTriggerPolicy/AiTriggerPolicy.h"
#include "backend/application/observability/operations/BusinessOperation.h"
#include <algorithm>
#include <utility>
namespace webserver::phase11::ai_chat_detail {
namespace {
constexpr std::size_t kPromptWindow=64;
bool upsertPreparationAudit(IAiGenerationAuditRepository &repository,
                            AiGenerationAudit audit)
{
    telemetry::BusinessOperation observation(telemetry::Series::AiPrepareAudit);
    const bool saved = repository.upsertGenerationAudit(std::move(audit));
    if (!saved)
        observation.fail();
    return saved;
}
std::vector<PromptMessage> buildPromptWindow(
    const std::vector<Message> &history, UserId aiUser)
{
    const auto begin = history.size() > kPromptWindow
                           ? history.end() - static_cast<std::ptrdiff_t>(kPromptWindow)
                           : history.begin();
    std::vector<PromptMessage> result;
    result.reserve(static_cast<std::size_t>(history.end() - begin));
    for (auto current = begin; current != history.end(); ++current)
    {
        result.push_back(
            {current->senderId == aiUser ? "assistant" : "user", current->body});
    }
    return result;
}

}
void prepareGeneration(const std::shared_ptr<AiChatService::State> &state,
                       AiChatRequest request,std::string traceparent) noexcept
{
    telemetry::BusinessOperation observation(telemetry::Series::AiPrepare,std::source_location::current(),true,traceparent);
    try
    {
        CancellationToken cancellation;
        bool stopping = false;
        AiGenerationAudit queuedAudit;
        {
            std::lock_guard lock(state->mutex);
            const auto found = state->active.find(request.requestId);
            if (found == state->active.end())
                return;
            cancellation = found->second.cancellation;
            stopping = state->stopping;
            queuedAudit = found->second.audit;
        }
        if (stopping || cancellation.cancelled())
        {
            observation.cancel();
            removeAndPublish(state, request.requestId, false, true,
                             "generation cancelled");
            return;
        }
        const auto requester = state->users->findUser(request.requester);
        const auto ai = state->users->findUser(request.aiUser);
        if (!requester)
            throw ApplicationError(ErrorCode::Unauthenticated,
                                   "requester is unavailable");
        if (!ai)
            throw ApplicationError(ErrorCode::InvalidArgument,
                                   "target user is not an enabled AI account");
        if (!state->conversations->findConversation(request.conversation))
            throw ApplicationError(ErrorCode::NotFound,
                                   "conversation not found");
        // 先确认外键实体存在，再记录尝试；这样客户端伪造不存在的 ID 不会把
        // PostgreSQL 外键错误误报成审计系统故障。
        if (!upsertPreparationAudit(*state->audits, std::move(queuedAudit)))
            throw ApplicationError(ErrorCode::Unavailable,
                                   "AI generation audit could not be created");
        if (requester->disabled)
            throw ApplicationError(ErrorCode::Unauthenticated,
                                   "requester is unavailable");
        if (ai->disabled || !ai->aiAccount)
            throw ApplicationError(ErrorCode::InvalidArgument,
                                   "target user is not an enabled AI account");
        if (!state->conversations->findMember(request.conversation,
                                               request.requester) ||
            !state->conversations->findMember(request.conversation,
                                               request.aiUser))
            throw ApplicationError(ErrorCode::Forbidden,
                                   "requester and AI must belong to the conversation");

        const auto members = state->conversations->listMembers(request.conversation);
        bool replyTargetsAi=false;
        if(request.trigger=="reply")replyTargetsAi=state->chat->visibleMessage(request.requester,request.conversation,request.replyToMessage).senderId==request.aiUser;
        AiTriggerPolicy::validate(request.trigger,request.prompt,*ai,replyTargetsAi);
        std::vector<UserId> recipients;
        recipients.reserve(members.size());
        for (const auto &member : members)
            recipients.push_back(member.userId);
        {
            std::lock_guard lock(state->mutex);
            const auto found = state->active.find(request.requestId);
            if (found == state->active.end())
                return;
            found->second.recipients = recipients;
        }

        (void)state->chat->sendText(
            request.requester, request.conversation, request.requestId,
            request.prompt, currentTime());
        auto history = [&]
        {
            telemetry::BusinessOperation historyObservation(
                telemetry::Series::AiPrepareHistory);
            return state->chat->historyBefore(
                request.requester, request.conversation, 0, kPromptWindow);
        }();
        // IMessageRepository guarantees ascending sequence order even when
        // fetching backwards. Cursor 0 means the newest bounded page; reversing
        // it would put an older question after the current user prompt.

        auto promptMessages = buildPromptWindow(history, request.aiUser);
        if (state->rag)
        {
            telemetry::BusinessOperation ragObservation(
                telemetry::Series::AiPrepareRag);
            auto context = state->rag->retrieveForGeneration(
                request.requestId, request.requester, request.conversation,
                request.prompt, state->config.ragMaxChunks, currentTime());
            if (!context.systemPrompt.empty())
                promptMessages.insert(promptMessages.begin(),
                                      {"system", std::move(context.systemPrompt)});
        }
        const auto [promptHash, promptBytes] = promptDigest(promptMessages);
        AiGenerationAudit promptAudit;
        {
            std::lock_guard lock(state->mutex);
            const auto found = state->active.find(request.requestId);
            if (found == state->active.end())
                return;
            found->second.audit.promptSha256 = promptHash;
            found->second.audit.promptBytes = promptBytes;
            ++found->second.audit.revision;
            promptAudit = found->second.audit;
        }
        if (!upsertPreparationAudit(*state->audits, std::move(promptAudit)))
            throw ApplicationError(ErrorCode::Unavailable,
                                   "AI prompt audit could not be updated");

        GenerationRequest generation;
        generation.requestId = request.requestId;
        generation.requester = request.requester;
        generation.messages = std::move(promptMessages);
        generation.maxOutputTokens = request.maxOutputTokens;
        state->models->generate(
            request.aiUser, std::move(generation),
            [state, id = request.requestId](std::string_view token)
            {
                onToken(state, id, token);
            },
            [state, id = request.requestId](GenerationCompletion completion)
            {
                onModelCompleted(state, id, std::move(completion));
            },
            cancellation,
            [state, id = request.requestId](const GenerationSelection &selection)
            {
                AiGenerationAudit selected;
                {
                    std::lock_guard lock(state->mutex);
                    const auto found = state->active.find(id);
                    if (found == state->active.end())
                        return;
                    auto &audit = found->second.audit;
                    audit.modelNodeId = selection.nodeId;
                    audit.modelVersionId = selection.versionId;
                    audit.runtime = selection.runtime;
                    audit.modelArtifact = selection.modelArtifact;
                    audit.adapterArtifact = selection.adapterArtifact;
                    audit.modelChecksum = selection.modelChecksum;
                    audit.status = AiGenerationStatus::Running;
                    audit.startedAt = currentTime();
                    ++audit.revision;
                    selected = audit;
                }
                if (!upsertPreparationAudit(*state->audits, std::move(selected)))
                    throw ApplicationError(ErrorCode::Unavailable,
                                           "AI model audit could not be updated");
            });
    }
    catch (const std::exception &failure)
    {
        observation.fail();
        removeAndPublish(state, request.requestId, false, false, failure.what());
    }
    catch (...)
    {
        observation.fail();
        removeAndPublish(state, request.requestId, false, false,
                         "AI generation setup failed");
    }
}

}
