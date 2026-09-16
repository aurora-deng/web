#include "server/sse/SseSessionManager.h"

#include "server/Reactor/ReactorGroup.h"
#include "server/sse/SseCodec.h"
#include "server/transport/OutboundTask.h"

#include <algorithm>

bool SseSessionManager::registerSession(
    SseClientId clientId,
    const std::shared_ptr<SseSession> &session,
    std::size_t reactorIndex,
    ConnectionKey key)
{
    if (clientId == 0 || !session || !key)
        return false;

    std::lock_guard<std::mutex> lock(mtx_);
    auto &bucket = sessions_[clientId];
    std::erase_if(bucket, [&](const Locator &item)
                  { return item.session.expired() || item.key == key; });
    bucket.push_back(Locator{session, reactorIndex, key});
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
    return post(liveTargets(clientId), event);
}

std::size_t SseSessionManager::broadcast(const SseEvent &event)
{
    return post(allLiveTargets(), event);
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
