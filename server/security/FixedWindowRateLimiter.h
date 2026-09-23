#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace webserver::security
{

/**
 * @brief 有界的按身份固定窗口限流器。
 *
 * 它像每位用户每分钟一张固定数量的取号票；票用完返回 false。表达到上限时不再
 * 接纳新的身份，避免攻击者用无限随机 uid 反过来撑爆限流器本身。
 */
class FixedWindowRateLimiter
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    FixedWindowRateLimiter(
        std::size_t requestsPerWindow = 120,
        std::chrono::milliseconds window = std::chrono::minutes(1),
        std::size_t maxIdentities = 65536)
        : requestsPerWindow_(std::max<std::size_t>(1, requestsPerWindow)),
          window_(std::max(window, std::chrono::milliseconds{1})),
          maxIdentities_(std::max<std::size_t>(1, maxIdentities))
    {
    }

    bool allow(std::uint64_t identity, TimePoint now = Clock::now())
    {
        if (identity == 0)
            return false;
        std::lock_guard lock(mutex_);
        auto found = entries_.find(identity);
        if (found == entries_.end())
        {
            prune(now);
            if (entries_.size() >= maxIdentities_)
                return false;
            found = entries_.emplace(identity, Entry{now, 0}).first;
        }
        auto &entry = found->second;
        if (now - entry.startedAt >= window_)
        {
            entry.startedAt = now;
            entry.count = 0;
        }
        if (entry.count >= requestsPerWindow_)
            return false;
        ++entry.count;
        return true;
    }

    std::size_t trackedIdentities() const
    {
        std::lock_guard lock(mutex_);
        return entries_.size();
    }

private:
    struct Entry
    {
        TimePoint startedAt;
        std::size_t count = 0;
    };

    void prune(TimePoint now)
    {
        for (auto it = entries_.begin(); it != entries_.end();)
        {
            if (now - it->second.startedAt >= window_ * 2)
                it = entries_.erase(it);
            else
                ++it;
        }
    }

    std::size_t requestsPerWindow_;
    std::chrono::milliseconds window_;
    std::size_t maxIdentities_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, Entry> entries_;
};

} // namespace webserver::security
