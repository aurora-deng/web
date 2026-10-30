#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"

namespace webserver::phase11 {
/** OutboxTypes: protocol/database independent business facts. */
struct OutboxEvent
{
    OutboxEventId id = 0;
    std::string type;
    std::string aggregateType;
    std::uint64_t aggregateId = 0;
    std::string payload;
    TimePoint createdAt{};
    std::optional<TimePoint> publishedAt;
};
}
