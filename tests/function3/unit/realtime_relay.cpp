#include "backend/application/modules/chat/application/RealtimeRelay/RealtimeRelay.h"
#include "backend/application/observability/BusinessMetrics/BusinessMetrics.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <vector>

using namespace webserver::phase11;

namespace {
void require(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
}

OutboxEvent event(OutboxEventId id, TimePoint time = {})
{
    OutboxEvent row;
    row.id = id;
    row.createdAt = time;
    return row;
}

class Feed final : public IRealtimeRelayRepository {
public:
    std::uint64_t relayHead() const override { return rows.back().position; }

    void stageRelayBatch(std::size_t limit) override
    {
        const auto count = std::min(limit, pending.size());
        for (std::size_t index = 0; index < count; ++index)
            rows.push_back({rows.back().position + 2, pending[index]});
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(count));
    }

    std::vector<RealtimeRelayEvent> readRelayBatch(std::uint64_t after, std::size_t limit) const override
    {
        std::vector<RealtimeRelayEvent> result;
        for (const auto &row : rows) {
            if (row.position > after && result.size() < limit)
                result.push_back(row);
        }
        return result;
    }

    std::vector<RealtimeRelayEvent> rows{{1, event(8)}};
    std::vector<OutboxEvent> pending;
};

class Sink final : public IOutboxEventSink {
public:
    bool publish(const OutboxEvent& event) override
    {
        if (event.id == blocked) return false;
        accepted.push_back(event.id);
        return true;
    }
    OutboxEventId blocked = 0;
    std::vector<OutboxEventId> accepted;
};
}

int main()
{
    Feed feed;
    Sink a, b;
    RealtimeRelay primary(feed, a), replica(feed, b);
    const auto now = TimePoint{} + std::chrono::seconds{10};
    feed.pending.push_back(event(12, now-std::chrono::seconds{3}));
    require(primary.dispatchPending(20, now).published == 1, "primary missed committed row");
    b.blocked = 12;
    const auto blocked = replica.dispatchPending(20, now);
    require(blocked.blocked && !blocked.published && blocked.oldestAttemptAge == std::chrono::seconds{3},
            "a failed delivery must keep the replica cursor and pending age");
    feed.pending.push_back(event(10, now));
    require(primary.dispatchPending(20, now).published == 1, "later commit with smaller Outbox ID was lost");
    b.blocked = 0;
    require(replica.dispatchPending(20, now).published == 2 && a.accepted == b.accepted
            && a.accepted == std::vector<OutboxEventId>{12, 10}, "replica retry did not replay the ordered suffix");
    require(!replica.dispatchPending(20, now).published, "completed rows were redelivered");

    using namespace webserver::business_metrics;
    observe(Database, 250000);
    observe(Database, 500000);
    observe(Queue, 250000);
    const auto data = text();
    require(data.find("nebula_database_operation_seconds_bucket{le=\"0.25\"} 1\n") != std::string::npos &&
            data.find("nebula_database_operation_seconds_bucket{le=\"+Inf\"} 2\n") != std::string::npos &&
            data.find("nebula_database_operation_seconds_count 2\n") != std::string::npos,
            "database histogram omitted its boundaries or included non-database work");
}
