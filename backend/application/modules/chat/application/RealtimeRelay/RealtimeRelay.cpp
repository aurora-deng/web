#include "RealtimeRelay.h"

namespace webserver::phase11 {

RealtimeRelay::RealtimeRelay(IRealtimeRelayRepository &repository, IOutboxEventSink &sink)
    : repository_(repository), sink_(sink), cursor_(repository.relayHead())
{
}

OutboxDispatchResult RealtimeRelay::dispatchPending(std::size_t limit, TimePoint now)
{
    repository_.stageRelayBatch(limit);
    OutboxDispatchResult result;
    for (const auto &row : repository_.readRelayBatch(cursor_, limit)) {
        result.beginAttempt(row.event, now);
        try {
            if (!sink_.publish(row.event)) {
                result.blocked = true;
                break;
            }
        } catch (...) {
            result.blocked = true;
            break;
        }
        cursor_ = row.position;
        ++result.published;
    }
    return result;
}

}
