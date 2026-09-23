#pragma once

#include "server/sse/SseEvent.h"
#include "server/sse/SseReplayBuffer.h"
#include "server/transport/ConnectionKey.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

class ReactorGroup;
class SseSession;

struct SseSessionManagerOptions
{
    std::size_t maxConnections = 5000;
    std::size_t maxConnectionsPerClient = 4;
    std::size_t replayEventsPerClient = 256;
    std::size_t replayClients = 4096;
};

struct SseMetricsSnapshot
{
    std::uint64_t registrations = 0;
    std::uint64_t rejectedRegistrations = 0;
    std::uint64_t publishedEvents = 0;
    std::uint64_t acceptedDeliveries = 0;
    std::uint64_t replayedEvents = 0;
    std::uint64_t replayGaps = 0;
};

/**
 * 全局 SSE 订阅目录。
 *
 * 一个 clientId 可以对应多个浏览器标签页；目录只保存 weak_ptr 和连接定位信息，
 * Connection::session 仍是会话的唯一真实所有者。
 */
class SseSessionManager
{
public:
    explicit SseSessionManager(SseSessionManagerOptions options = {});
    void bind(ReactorGroup *group) noexcept { reactorGroup_ = group; }

    bool registerSession(SseClientId clientId,
                         const std::shared_ptr<SseSession> &session,
                         std::size_t reactorIndex,
                         ConnectionKey key);
    bool unregister(SseClientId clientId, ConnectionKey key);

    /** 向某个 clientId 的全部在线连接发布；返回进入 Reactor 出站邮箱的连接数。 */
    std::size_t publish(SseClientId clientId, const SseEvent &event);
    /** 向全部在线 SSE 连接发布；返回进入 Reactor 出站邮箱的连接数。 */
    std::size_t broadcast(const SseEvent &event);
    /** 给刚重连的单条连接补发游标之后的事件；未知游标发送 replay-reset。 */
    std::size_t replayTo(SseClientId clientId,
                         ConnectionKey key,
                         const std::string &lastEventId);
    std::size_t onlineCount() const;
    std::size_t historyEventCount() const { return replay_.eventCount(); }
    SseMetricsSnapshot metrics() const;

private:
    struct Locator
    {
        std::weak_ptr<SseSession> session;
        std::size_t reactorIndex = 0;
        ConnectionKey key{};
    };

    std::vector<Locator> liveTargets(SseClientId clientId);
    std::vector<Locator> allLiveTargets();
    std::size_t post(const std::vector<Locator> &targets,
                     const SseEvent &event);

    mutable std::mutex mtx_;
    std::unordered_map<SseClientId, std::vector<Locator>> sessions_;
    ReactorGroup *reactorGroup_ = nullptr;
    SseSessionManagerOptions options_;
    SseReplayBuffer replay_;
    std::atomic<std::uint64_t> registrations_{0};
    std::atomic<std::uint64_t> rejectedRegistrations_{0};
    std::atomic<std::uint64_t> publishedEvents_{0};
    std::atomic<std::uint64_t> acceptedDeliveries_{0};
    std::atomic<std::uint64_t> replayedEvents_{0};
    std::atomic<std::uint64_t> replayGaps_{0};
};
