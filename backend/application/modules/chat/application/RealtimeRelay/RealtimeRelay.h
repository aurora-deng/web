#pragma once

#include "backend/application/modules/messaging/application/OutboxDispatcher/OutboxDispatcher.h"
#include "backend/application/modules/chat/ports/IRealtimeRelayRepository/IRealtimeRelayRepository.h"

namespace webserver::phase11 {

// One cursor per process. Process restart disconnects sockets, which resume via history.
class RealtimeRelay final {
public:
    RealtimeRelay(IRealtimeRelayRepository &repository, IOutboxEventSink &sink);
    OutboxDispatchResult dispatchPending(std::size_t limit, TimePoint now);

private:
    IRealtimeRelayRepository &repository_;
    IOutboxEventSink &sink_;
    std::uint64_t cursor_;
};

}
