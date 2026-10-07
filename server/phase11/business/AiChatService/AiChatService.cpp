#include "server/phase11/business/AiChatService/AiChatService.h"

#include "server/phase11/business/ApplicationError/ApplicationError.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace webserver::phase11
{
namespace
{

constexpr std::size_t kMaxPersistedAnswerBytes = 16U * 1024U;
constexpr std::size_t kHistoryLimit = 500;
constexpr std::size_t kPromptWindow = 64;

TimePoint currentTime()
{
    return std::chrono::system_clock::now();
}

} // namespace

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
    };

    State(ChatService &chatService,
          IUserRepository &userRepository,
          IConversationRepository &conversationRepository,
          ModelRouter &modelRouter,
          DatabaseExecutor &executor,
          IAiGenerationEventSink &eventSink,
          AiChatServiceConfig serviceConfig)
        : chat(&chatService), users(&userRepository),
          conversations(&conversationRepository), models(&modelRouter),
          databaseExecutor(&executor), events(&eventSink),
          config(std::move(serviceConfig))
    {
    }

    ChatService *chat;
    IUserRepository *users;
    IConversationRepository *conversations;
    ModelRouter *models;
    DatabaseExecutor *databaseExecutor;
    IAiGenerationEventSink *events;
    AiChatServiceConfig config;

    std::mutex mutex;
    std::condition_variable changed;
    std::unordered_map<std::string, ActiveGeneration> active;
    AiChatMetricsSnapshot metrics;
    std::thread watchdog;
    bool stopping = false;
};

namespace
{

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

void removeAndPublish(const std::shared_ptr<AiChatService::State> &state,
                      const std::string &generationId,
                      bool success,
                      bool cancelled,
                      std::string error,
                      std::optional<Message> message = {}) noexcept
{
    AiCompletedEvent event;
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
        state->active.erase(found);
        if (success)
            ++state->metrics.succeeded;
        else if (cancelled)
            ++state->metrics.cancelled;
        else
            ++state->metrics.failed;
    }
    state->changed.notify_all();
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
            current = state->active.erase(current);
            ++state->metrics.failed;
            ++state->metrics.timedOut;
        }

        lock.unlock();
        for (const auto &cancellation : cancellations)
            cancellation.cancel();
        for (auto &event : events)
            publishCompleted(state, std::move(event));
        lock.lock();
    }
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

void onToken(const std::shared_ptr<AiChatService::State> &state,
             const std::string &generationId,
             std::string_view token) noexcept
{
    AiTokenEvent event;
    CancellationToken cancellation;
    bool cancelForLimit = false;
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
        }
    }

    if (cancelForLimit)
    {
        cancellation.cancel();
        removeAndPublish(state, generationId, false, false,
                         "AI response exceeds the message size limit");
        return;
    }
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

void prepareGeneration(const std::shared_ptr<AiChatService::State> &state,
                       AiChatRequest request) noexcept
{
    try
    {
        CancellationToken cancellation;
        bool stopping = false;
        {
            std::lock_guard lock(state->mutex);
            const auto found = state->active.find(request.requestId);
            if (found == state->active.end())
                return;
            cancellation = found->second.cancellation;
            stopping = state->stopping;
        }
        if (stopping || cancellation.cancelled())
        {
            removeAndPublish(state, request.requestId, false, true,
                             "generation cancelled");
            return;
        }

        const auto requester = state->users->findUser(request.requester);
        const auto ai = state->users->findUser(request.aiUser);
        if (!requester || requester->disabled)
            throw ApplicationError(ErrorCode::Unauthenticated,
                                   "requester is unavailable");
        if (!ai || ai->disabled || !ai->aiAccount)
            throw ApplicationError(ErrorCode::InvalidArgument,
                                   "target user is not an enabled AI account");
        if (!state->conversations->findMember(request.conversation,
                                               request.requester) ||
            !state->conversations->findMember(request.conversation,
                                               request.aiUser))
            throw ApplicationError(ErrorCode::Forbidden,
                                   "requester and AI must belong to the conversation");

        const auto members = state->conversations->listMembers(request.conversation);
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
        const auto history = state->chat->history(
            request.requester, request.conversation, 0, kHistoryLimit);

        GenerationRequest generation;
        generation.requestId = request.requestId;
        generation.requester = request.requester;
        generation.messages = buildPromptWindow(history, request.aiUser);
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
            cancellation);
    }
    catch (const std::exception &failure)
    {
        removeAndPublish(state, request.requestId, false, false, failure.what());
    }
    catch (...)
    {
        removeAndPublish(state, request.requestId, false, false,
                         "AI generation setup failed");
    }
}

} // namespace

AiChatService::AiChatService(ChatService &chat,
                             IUserRepository &users,
                             IConversationRepository &conversations,
                             ModelRouter &models,
                             DatabaseExecutor &databaseExecutor,
                             IAiGenerationEventSink &events,
                             AiChatServiceConfig config)
    : state_(std::make_shared<State>(chat, users, conversations, models,
                                     databaseExecutor, events,
                                     std::move(config)))
{
    if (state_->config.maxActiveGenerations == 0 ||
        state_->config.maxActivePerUser == 0 ||
        state_->config.maxActivePerAiAccount == 0 ||
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
        if (!state->active.emplace(request.requestId, std::move(active)).second)
            throw ApplicationError(ErrorCode::Conflict,
                                   "generation id is already active");
        ++state->metrics.accepted;
    }
    state->changed.notify_all();

    const auto generationId = request.requestId;
    if (!state->databaseExecutor->submit(
            [state, request = std::move(request)]() mutable
            {
                prepareGeneration(state, std::move(request));
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
            current = state->active.erase(current);
            ++state->metrics.cancelled;
        }
    }
    state->changed.notify_all();
    for (const auto &cancellation : cancellations)
        cancellation.cancel();
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
