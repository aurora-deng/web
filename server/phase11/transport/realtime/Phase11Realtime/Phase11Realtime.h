#pragma once

#include "server/phase11/business/AiChatService/AiChatService.h"
#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/business/OutboxDispatcher/OutboxDispatcher.h"
#include "server/phase11/business/BusinessInterfaces/BusinessInterfaces.h"
#include "server/phase11/runtime/BoundedExecutor/BoundedExecutor.h"

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

class SseSessionManager;
class WebSocketDispatcher;
class WebSocketSessionManager;

namespace webserver::phase11::transport
{

/** chat.send / chat.ack / chat.read 的 WebSocket 业务适配器。 */
class Phase11Realtime final
{
public:
    Phase11Realtime(IChatBusiness &chat, DatabaseExecutor &databaseExecutor,
                    IAiChatBusiness *aiChat = nullptr)
        : chat_(chat), databaseExecutor_(databaseExecutor), aiChat_(aiChat)
    {
    }

    void registerHandlers(WebSocketDispatcher &dispatcher);

private:
    template<class Operation>
    auto runDatabase(Operation &&operation)
        -> std::invoke_result_t<std::decay_t<Operation>>
    {
        using Result = std::invoke_result_t<std::decay_t<Operation>>;
        auto completion = std::make_shared<std::promise<Result>>();
        auto result = completion->get_future();
        if (!databaseExecutor_.submit(
                [completion,
                 operation = std::forward<Operation>(operation)]() mutable
                {
                    try
                    {
                        if constexpr (std::is_void_v<Result>)
                        {
                            operation();
                            completion->set_value();
                        }
                        else
                            completion->set_value(operation());
                    }
                    catch (...)
                    {
                        completion->set_exception(std::current_exception());
                    }
                }))
            throw ApplicationError(ErrorCode::Unavailable,
                                   "database service is busy");
        return result.get();
    }

    IChatBusiness &chat_;
    DatabaseExecutor &databaseExecutor_;
    IAiChatBusiness *aiChat_;
};

/** 把协议无关的 AI token/完成事件编码成可重放的 SSE 事件。 */
class Phase11AiEventSink final : public IAiGenerationEventSink
{
public:
    explicit Phase11AiEventSink(SseSessionManager &sse) : sse_(sse) {}

    void token(const AiTokenEvent &event) override;
    void completed(const AiCompletedEvent &event) override;

private:
    SseSessionManager &sse_;
};

/** 把已提交的 Outbox 事件翻译成 WebSocket 消息和 SSE 通知。 */
class Phase11RealtimeOutboxSink final : public IOutboxEventSink
{
public:
    Phase11RealtimeOutboxSink(IMessageRepository &messages,
                              IConversationRepository &conversations,
                              WebSocketSessionManager &webSockets,
                              SseSessionManager &sse)
        : messages_(messages), conversations_(conversations),
          webSockets_(webSockets), sse_(sse)
    {
    }

    bool publish(const OutboxEvent &event) override;

private:
    IMessageRepository &messages_;
    IConversationRepository &conversations_;
    WebSocketSessionManager &webSockets_;
    SseSessionManager &sse_;
};

/**
 * Outbox 专用后台搬运线程。数据库慢时只阻塞这名搬运工，不占 Reactor、HTTP Worker
 * 或 WebSocket Worker；失败事件保留在表中，下轮继续尝试。
 */
class Phase11OutboxPump final
{
public:
    Phase11OutboxPump(IOutboxRepository &outbox, IOutboxEventSink &sink);
    ~Phase11OutboxPump();

    Phase11OutboxPump(const Phase11OutboxPump &) = delete;
    Phase11OutboxPump &operator=(const Phase11OutboxPump &) = delete;

    void stop() noexcept;

private:
    void run() noexcept;

    OutboxDispatcher dispatcher_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
};

} // namespace webserver::phase11::transport
