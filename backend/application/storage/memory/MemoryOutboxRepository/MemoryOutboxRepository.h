#pragma once
#include "backend/application/storage/memory/MemoryState/MemoryState.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
namespace webserver::phase11 {
/** Outbox in-memory adapter. Facade holds the shared lock during calls. */
class MemoryOutboxRepository final {
public:
    static bool markPublishedBatch(storage_detail::MemoryState &memory, const std::vector<OutboxEventId> &ids, TimePoint now);
    static OutboxEvent appendOutbox(storage_detail::MemoryState &memory, OutboxEvent event);
    static std::vector<OutboxEvent> pendingOutbox(const storage_detail::MemoryState &memory, std::size_t limit);
    static bool markPublished(storage_detail::MemoryState &memory, OutboxEventId id, TimePoint now);
};
}
