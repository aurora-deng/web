#include "server/sse/SseReplayBuffer.h"

#include <algorithm>
#include <iterator>
#include <limits>

SseReplayBuffer::SseReplayBuffer(
    std::size_t eventsPerClient,
    std::size_t maxClients)
    : eventsPerClient_(std::max<std::size_t>(1, eventsPerClient)),
      maxClients_(std::max<std::size_t>(1, maxClients))
{
}

void SseReplayBuffer::evictOldestClient()
{
    if (histories_.empty())
        return;
    auto oldest = histories_.begin();
    for (auto current = std::next(histories_.begin());
         current != histories_.end(); ++current)
    {
        if (current->second.lastTouched < oldest->second.lastTouched)
            oldest = current;
    }
    histories_.erase(oldest);
}

SseEvent SseReplayBuffer::append(SseClientId clientId, SseEvent event)
{
    if (clientId == 0)
        return event;

    std::lock_guard lock(mutex_);
    auto found = histories_.find(clientId);
    if (found == histories_.end())
    {
        if (histories_.size() >= maxClients_)
            evictOldestClient();
        found = histories_.try_emplace(clientId).first;
    }
    if (event.id.empty())
        event.id = std::to_string(nextEventId_++);
    else if (nextEventId_ != std::numeric_limits<std::uint64_t>::max())
        ++nextEventId_; // 外部 id 也推进全局次序，避免触摸序号长期不变。

    auto &history = found->second;
    history.lastTouched = touchSequence_++;
    history.events.push_back(event);
    while (history.events.size() > eventsPerClient_)
        history.events.pop_front();
    return event;
}

SseReplayResult SseReplayBuffer::replayAfter(
    SseClientId clientId,
    const std::string &lastEventId) const
{
    if (clientId == 0 || lastEventId.empty())
        return {};
    std::lock_guard lock(mutex_);
    const auto found = histories_.find(clientId);
    if (found == histories_.end() || found->second.events.empty())
        return {{}, true};

    const auto &events = found->second.events;
    const auto cursor = std::find_if(events.begin(), events.end(),
                                     [&](const SseEvent &event)
                                     { return event.id == lastEventId; });
    if (cursor == events.end())
        return {{}, true};
    return {{std::next(cursor), events.end()}, false};
}

std::size_t SseReplayBuffer::eventCount() const
{
    std::lock_guard lock(mutex_);
    std::size_t total = 0;
    for (const auto &[_, history] : histories_)
        total += history.events.size();
    return total;
}
