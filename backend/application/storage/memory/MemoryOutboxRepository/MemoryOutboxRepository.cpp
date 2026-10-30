#include <unordered_set>
#include "backend/application/storage/memory/MemoryOutboxRepository/MemoryOutboxRepository.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>
namespace webserver::phase11 {
using storage_detail::orderedUsers;
using storage_detail::UserPair;
using storage_detail::MemberKey;
using storage_detail::IdempotencyKey;
OutboxEvent MemoryOutboxRepository::appendOutbox(storage_detail::MemoryState &memory, OutboxEvent event)
{
    event.id = memory.nextOutboxEventId++;
    memory.outbox.emplace(event.id, event);
    return event;
}

std::vector<OutboxEvent> MemoryOutboxRepository::pendingOutbox(const storage_detail::MemoryState &memory, std::size_t limit)
{
    std::vector<OutboxEvent> result;
    for (const auto &[_, event] : memory.outbox)
    {
        if (!event.publishedAt)
            result.push_back(event);
    }
    std::sort(result.begin(), result.end(),
              [](const auto &left, const auto &right) {
                  return left.id < right.id;
              });
    if (result.size() > limit)
        result.resize(limit);
    return result;
}

bool MemoryOutboxRepository::markPublished(storage_detail::MemoryState &memory, OutboxEventId id, TimePoint now)
{
    const auto found = memory.outbox.find(id);
    if (found == memory.outbox.end())
        return false;
    found->second.publishedAt = now;
    return true;
}

}

namespace webserver::phase11 {
bool MemoryOutboxRepository::markPublishedBatch(storage_detail::MemoryState &memory,const std::vector<OutboxEventId> &ids,TimePoint now) {
    if(ids.empty())return true;
    if(ids.size()>1000 || std::unordered_set<OutboxEventId>(ids.begin(),ids.end()).size()!=ids.size())return false;
    for(auto id:ids)if(!memory.outbox.contains(id))return false;
    for(auto id:ids)memory.outbox.at(id).publishedAt=now;
    return true;
}
}
