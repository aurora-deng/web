#include "server/phase11/business/OutboxDispatcher/OutboxDispatcher.h"

#include <algorithm>

namespace webserver::phase11
{

OutboxDispatchResult OutboxDispatcher::dispatchPending(
    std::size_t limit, TimePoint now)
{
    if (limit == 0 || limit > 1'000)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "outbox batch limit must be between 1 and 1000");

    OutboxDispatchResult result;
    for (const auto &event : outbox_.pendingOutbox(limit))
    {
        ++result.attempted;
        result.oldestAttemptAge = std::max(
            result.oldestAttemptAge,
            std::chrono::duration_cast<std::chrono::microseconds>(
                now > event.createdAt ? now - event.createdAt
                                      : TimePoint::duration::zero()));
        bool accepted = false;
        try
        {
            accepted = sink_.publish(event);
        }
        catch (...)
        {
            accepted = false;
        }
        if (!accepted)
        {
            result.blocked = true;
            break;
        }
        if (!outbox_.markPublished(event.id, now))
            throw ApplicationError(ErrorCode::Unavailable,
                                   "published outbox event could not be acknowledged");
        ++result.published;
    }
    return result;
}

} // namespace webserver::phase11
