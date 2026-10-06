#pragma once

#include "server/phase11/business/AiChatService/AiChatService.h"
#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/business/OutboxDispatcher/OutboxDispatcher.h"
#include "server/phase11/business/BusinessInterfaces/BusinessInterfaces.h"
#include "server/phase11/runtime/BoundedExecutor/BoundedExecutor.h"

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <future>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

class SseSessionManager;
class WebSocketDispatcher;
class WebSocketSessionManager;

namespace webserver::phase11::transport
{

/** chat.send / chat.ack / chat.read / chat.recall 的 WebSocket 业务适配器。 */
class Phase11Realtime final
{
public:
    Phase11Realtime(IChatBusiness &chat, DatabaseExecutor &databaseExecutor,
                    IAiChatBusiness *aiChat = nullptr,
                    std::function<void()> outboxCommitted = {})
        : chat_(chat), databaseExecutor_(databaseExecutor), aiChat_(aiChat),
          outboxCommitted_(std::move(outboxCommitted))
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
    std::function<void()> outboxCommitted_;
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

/**
 * 把 WebSocket 多设备连接数变化投递到数据库执行器。Reactor 只做一次无阻塞 submit，
 * last-active 与 Outbox 写入由数据库 Worker 完成。
 */
class Phase11PresenceBridge final
{
public:
    Phase11PresenceBridge(ISocialBusiness &social,
                          DatabaseExecutor &databaseExecutor,
                          WebSocketSessionManager &webSockets,
                          std::function<void()> outboxCommitted = {});
    ~Phase11PresenceBridge();

    Phase11PresenceBridge(const Phase11PresenceBridge &) = delete;
    Phase11PresenceBridge &operator=(const Phase11PresenceBridge &) = delete;

private:
    WebSocketSessionManager &webSockets_;
};

/** 把已提交的 Outbox 事件翻译成 WebSocket 消息和 SSE 通知。 */
class Phase11RealtimeOutboxSink final : public IOutboxEventSink
{
public:
    Phase11RealtimeOutboxSink(IMessageRepository &messages,
                              IConversationRepository &conversations,
                              ISocialRepository &social,
                              WebSocketSessionManager &webSockets,
                              SseSessionManager &sse)
        : messages_(messages), conversations_(conversations), social_(social),
          webSockets_(webSockets), sse_(sse)
    {
    }

    bool publish(const OutboxEvent &event) override;

private:
    IMessageRepository &messages_;
    IConversationRepository &conversations_;
    ISocialRepository &social_;
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
    struct Snapshot
    {
        std::uint64_t cycles = 0;
        std::uint64_t wakeups = 0;
        std::uint64_t attempted = 0;
        std::uint64_t published = 0;
        std::uint64_t blocked = 0;
        std::uint64_t failures = 0;
        std::size_t lastBatch = 0;
        std::size_t batchHighWatermark = 0;
        std::uint64_t oldestAttemptAgeMicroseconds = 0;
    };

    Phase11OutboxPump(IOutboxRepository &outbox, IOutboxEventSink &sink);
    ~Phase11OutboxPump();

    Phase11OutboxPump(const Phase11OutboxPump &) = delete;
    Phase11OutboxPump &operator=(const Phase11OutboxPump &) = delete;

    void stop() noexcept;
    /** 事务提交后叫醒搬运线程；250 ms 轮询只保留为漏通知兜底。 */
    void notifyCommitted() noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;

private:
    void run() noexcept;

    OutboxDispatcher dispatcher_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::uint64_t wakeGeneration_ = 0;
    std::thread worker_;
    std::atomic<std::uint64_t> cycles_{0};
    std::atomic<std::uint64_t> wakeups_{0};
    std::atomic<std::uint64_t> attempted_{0};
    std::atomic<std::uint64_t> published_{0};
    std::atomic<std::uint64_t> blocked_{0};
    std::atomic<std::uint64_t> failures_{0};
    std::atomic<std::size_t> lastBatch_{0};
    std::atomic<std::size_t> batchHighWatermark_{0};
    std::atomic<std::uint64_t> oldestAttemptAgeMicroseconds_{0};
};

} // namespace webserver::phase11::transport
