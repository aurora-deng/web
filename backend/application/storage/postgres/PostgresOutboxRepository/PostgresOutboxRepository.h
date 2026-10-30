#pragma once
#include <memory>
#include "backend/application/domain/OutboxTypes/OutboxTypes.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
#include "backend/application/storage/postgres/PostgresOutboxWriter/PostgresOutboxWriter.h"
namespace webserver::phase11 {
/** Outbox SQL adapter. Uses the caller's connection; never borrows another lease. */
class PostgresOutboxRepository final {
public:
    static bool markPublishedBatch(PGconn *connection, const std::vector<OutboxEventId> &ids, TimePoint now);
    static OutboxEvent appendOutbox(PGconn *connection, bool ownsTransaction, const std::shared_ptr<PostgresOutboxWriter>& writer, OutboxEvent event);
    static std::vector<OutboxEvent> pendingOutbox(PGconn *connection, std::size_t limit);
    static bool markPublished(PGconn *connection, OutboxEventId id, TimePoint now);
};
}
