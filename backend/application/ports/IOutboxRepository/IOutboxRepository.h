#pragma once
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include "backend/application/domain/OutboxTypes/OutboxTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace webserver::phase11 {
class IOutboxRepository
{
public:
    virtual ~IOutboxRepository() = default;
    virtual OutboxEvent appendOutbox(OutboxEvent event) = 0;
    [[nodiscard]] virtual std::vector<OutboxEvent> pendingOutbox(
        std::size_t limit) const = 0;
    virtual bool markPublished(OutboxEventId id, TimePoint now) = 0;
    /** Mark only a successfully published prefix. Native stores override this
     * with one atomic operation; the fallback preserves older adapters. */
    virtual bool markPublishedBatch(const std::vector<OutboxEventId> &ids, TimePoint now)
    {
        for (const auto id : ids)
            if (!markPublished(id, now)) return false;
        return true;
    }
};
}
