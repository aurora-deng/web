#pragma once

#include "server/sse/SseEvent.h"
#include "server/transport/ConnectionKey.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

class ReactorGroup;
class SseSession;

/**
 * 全局 SSE 订阅目录。
 *
 * 一个 clientId 可以对应多个浏览器标签页；目录只保存 weak_ptr 和连接定位信息，
 * Connection::session 仍是会话的唯一真实所有者。
 */
class SseSessionManager
{
public:
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
    std::size_t onlineCount() const;

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
};
