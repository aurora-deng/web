#include "backend/application/business/OutboxDispatcher/OutboxDispatcher.h"

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
    std::vector<OutboxEventId> acceptedIds;
    acceptedIds.reserve(limit);
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
        acceptedIds.push_back(event.id);
    }
    // One durable acknowledgement for the accepted prefix saves a synchronous
    // database commit per event. A crash before this point replays that prefix;
    // clients/Inbox still deduplicate by the original event/message identity.
    if (!acceptedIds.empty() && !outbox_.markPublishedBatch(acceptedIds, now))
        throw ApplicationError(ErrorCode::Unavailable,
                               "published outbox batch could not be acknowledged");
    result.published = acceptedIds.size();
    return result;
}

} // namespace webserver::phase11
