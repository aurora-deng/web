#pragma once

#include "backend/application/modules/messaging/domain/OutboxTypes/OutboxTypes.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace webserver::phase11 {

struct RealtimeRelayEvent {
    std::uint64_t position;
    OutboxEvent event;
};

class IRealtimeRelayRepository {
public:
    virtual ~IRealtimeRelayRepository() = default;
    virtual std::uint64_t relayHead() const = 0;
    virtual void stageRelayBatch(std::size_t limit) = 0;
    virtual std::vector<RealtimeRelayEvent> readRelayBatch(std::uint64_t after, std::size_t limit) const = 0;
};

}
