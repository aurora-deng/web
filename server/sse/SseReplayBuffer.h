#pragma once

#include "server/sse/SseEvent.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/** 一次 Last-Event-ID 查询结果；gap=true 表示请求的游标已经不在保留窗口内。 */
struct SseReplayResult
{
    std::vector<SseEvent> events;
    bool gap = false;
};

/**
 * @brief 有界的进程内 SSE 事件历史。
 *
 * 每个 clientId 是一本只保留最后 N 页的流水账。客户端带 Last-Event-ID 重连时，
 * 找到那一页就补发后续内容；找不到则明确报告 gap，不能假装消息从未丢失。
 */
class SseReplayBuffer
{
public:
    explicit SseReplayBuffer(
        std::size_t eventsPerClient = 256,
        std::size_t maxClients = 4096);

    /** 保存事件并在 id 为空时分配单调递增 id，返回实际保存的事件。 */
    SseEvent append(SseClientId clientId, SseEvent event);
    [[nodiscard]] SseReplayResult replayAfter(
        SseClientId clientId, const std::string &lastEventId) const;
    [[nodiscard]] std::size_t eventCount() const;

private:
    struct History
    {
        std::deque<SseEvent> events;
        std::uint64_t lastTouched = 0;
    };

    void evictOldestClient();

    std::size_t eventsPerClient_;
    std::size_t maxClients_;
    mutable std::mutex mutex_;
    std::uint64_t nextEventId_ = 1;
    std::uint64_t touchSequence_ = 1;
    std::unordered_map<SseClientId, History> histories_;
};
