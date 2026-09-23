#include "server/sse/SseSessionManager.h"

#include "server/Reactor/ReactorGroup.h"
#include "server/sse/SseCodec.h"
#include "server/transport/OutboundTask.h"

#include <algorithm>
#include <utility>

SseSessionManager::SseSessionManager(SseSessionManagerOptions options)
    : options_(std::move(options)),
      replay_(options_.replayEventsPerClient, options_.replayClients)
{
    options_.maxConnections = std::max<std::size_t>(1, options_.maxConnections);
    options_.maxConnectionsPerClient =
        std::max<std::size_t>(1, options_.maxConnectionsPerClient);
}

bool SseSessionManager::registerSession(
    SseClientId clientId,
    const std::shared_ptr<SseSession> &session,
    std::size_t reactorIndex,
    ConnectionKey key)
{
    if (clientId == 0 || !session || !key)
        return false;

    std::lock_guard<std::mutex> lock(mtx_);
    std::size_t total = 0;
    // 先清扫所有用户的过期 weak_ptr，再计算全局连接配额。否则已经断开的旧连接
    // 仍会占着“座位”，最终让健康的新订阅被错误拒绝。
    for (auto it = sessions_.begin(); it != sessions_.end();)
    {
        auto &entries = it->second;
        std::erase_if(entries, [](const Locator &item)
                      { return item.session.expired(); });
        if (entries.empty())
        {
            it = sessions_.erase(it);
            continue;
        }
        total += entries.size();
        ++it;
    }

    auto &bucket = sessions_[clientId];
    const auto duplicate = std::find_if(
        bucket.begin(), bucket.end(), [&](const Locator &item)
        { return item.key == key; });
    if (duplicate != bucket.end())
    {
        rejectedRegistrations_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (total >= options_.maxConnections ||
        bucket.size() >= options_.maxConnectionsPerClient)
    {
        if (bucket.empty())
            sessions_.erase(clientId);
        rejectedRegistrations_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    bucket.push_back(Locator{session, reactorIndex, key});
    registrations_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool SseSessionManager::unregister(SseClientId clientId, ConnectionKey key)
{
    if (clientId == 0 || !key)
        return false;

    std::lock_guard<std::mutex> lock(mtx_);
    const auto found = sessions_.find(clientId);
    if (found == sessions_.end())
        return false;

    auto &bucket = found->second;
    const auto oldSize = bucket.size();
    std::erase_if(bucket, [&](const Locator &item)
                  { return item.session.expired() || item.key == key; });
    const bool removed = bucket.size() != oldSize;
    if (bucket.empty())
        sessions_.erase(found);
    return removed;
}

std::vector<SseSessionManager::Locator>
SseSessionManager::liveTargets(SseClientId clientId)
{
    std::lock_guard<std::mutex> lock(mtx_);
    const auto found = sessions_.find(clientId);
    if (found == sessions_.end())
        return {};

    auto &bucket = found->second;
    std::erase_if(bucket, [](const Locator &item)
                  { return item.session.expired(); });
    if (bucket.empty())
    {
        sessions_.erase(found);
        return {};
    }
    return bucket;
}

std::vector<SseSessionManager::Locator>
SseSessionManager::allLiveTargets()
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<Locator> result;
    for (auto it = sessions_.begin(); it != sessions_.end();)
    {
        auto &bucket = it->second;
        std::erase_if(bucket, [](const Locator &item)
                      { return item.session.expired(); });
        if (bucket.empty())
        {
            it = sessions_.erase(it);
            continue;
        }
        result.insert(result.end(), bucket.begin(), bucket.end());
        ++it;
    }
    return result;
}

std::size_t SseSessionManager::post(const std::vector<Locator> &targets,
                                    const SseEvent &event)
{
    if (!reactorGroup_ || targets.empty())
        return 0;

    auto bytes = std::make_shared<const std::string>(
        SseCodec::encodeChunk(SseCodec::encodeEvent(event)));
    std::size_t accepted = 0;
    for (const auto &target : targets)
    {
        if (reactorGroup_->postOutbound(
                target.reactorIndex,
                target.key.fd,
                target.key.connId,
                OutboundTask::sharedEncoded(bytes)) == EnqueueResult::Ok)
        {
            ++accepted;
        }
    }
    return accepted;
}

std::size_t SseSessionManager::publish(SseClientId clientId,
                                       const SseEvent &event)
{
    const auto stored = replay_.append(clientId, event);
    publishedEvents_.fetch_add(1, std::memory_order_relaxed);
    const auto accepted = post(liveTargets(clientId), stored);
    acceptedDeliveries_.fetch_add(accepted, std::memory_order_relaxed);
    return accepted;
}

std::size_t SseSessionManager::broadcast(const SseEvent &event)
{
    std::vector<SseClientId> clients;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        clients.reserve(sessions_.size());
        for (const auto &[clientId, _] : sessions_)
            clients.push_back(clientId);
    }
    std::size_t accepted = 0;
    for (const auto clientId : clients)
        accepted += publish(clientId, event);
    return accepted;
}

std::size_t SseSessionManager::replayTo(
    SseClientId clientId,
    ConnectionKey key,
    const std::string &lastEventId)
{
    Locator target;
    bool foundTarget = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto found = sessions_.find(clientId);
        if (found != sessions_.end())
        {
            for (const auto &candidate : found->second)
            {
                if (candidate.key == key && !candidate.session.expired())
                {
                    target = candidate;
                    foundTarget = true;
                    break;
                }
            }
        }
    }
    if (!foundTarget)
        return 0;

    const auto replay = replay_.replayAfter(clientId, lastEventId);
    if (replay.gap)
    {
        replayGaps_.fetch_add(1, std::memory_order_relaxed);
        SseEvent reset;
        reset.eventName = "replay-reset";
        reset.data = "Last-Event-ID is outside the retained history";
        return post({target}, reset);
    }

    std::size_t accepted = 0;
    for (const auto &event : replay.events)
        accepted += post({target}, event);
    replayedEvents_.fetch_add(accepted, std::memory_order_relaxed);
    return accepted;
}

std::size_t SseSessionManager::onlineCount() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::size_t count = 0;
    for (const auto &[_, bucket] : sessions_)
        for (const auto &item : bucket)
            if (!item.session.expired())
                ++count;
    return count;
}

SseMetricsSnapshot SseSessionManager::metrics() const
{
    return {
        registrations_.load(std::memory_order_relaxed),
        rejectedRegistrations_.load(std::memory_order_relaxed),
        publishedEvents_.load(std::memory_order_relaxed),
        acceptedDeliveries_.load(std::memory_order_relaxed),
        replayedEvents_.load(std::memory_order_relaxed),
        replayGaps_.load(std::memory_order_relaxed)};
}
