#pragma once

#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/ports/Ports/Ports.h"

#include <cstddef>

namespace webserver::phase11
{

/** WebSocket/SSE/gRPC 推送层实现这个端口；领域层不依赖具体连接对象。 */
class IOutboxEventSink
{
public:
    virtual ~IOutboxEventSink() = default;

    /** 返回 true 表示事件已被推送层接收；false 表示应保留在 Outbox 中稍后重试。 */
    [[nodiscard]] virtual bool publish(const OutboxEvent &event) = 0;
};

struct OutboxDispatchResult
{
    std::size_t attempted = 0;
    std::size_t published = 0;
    bool blocked = false;
};

/**
 * 按事件 ID 顺序搬运 Outbox。发布成功后才标记 published；失败立即停在当前事件，避免
 * 后面的通知越过它。若“推送成功、标记失败”，事件会在下次被再次投递，因此接收端必须
 * 按 OutboxEvent.id 去重——这是标准的 at-least-once（至少一次）语义。
 */
class OutboxDispatcher
{
public:
    OutboxDispatcher(IOutboxRepository &outbox, IOutboxEventSink &sink)
        : outbox_(outbox), sink_(sink)
    {
    }

    [[nodiscard]] OutboxDispatchResult dispatchPending(
        std::size_t limit, TimePoint now);

private:
    IOutboxRepository &outbox_;
    IOutboxEventSink &sink_;
};

} // namespace webserver::phase11
