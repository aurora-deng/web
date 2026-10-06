#pragma once

#include "server/phase11/ports/Ports/Ports.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace webserver::phase11
{

struct LoginRateLimitPolicy
{
    std::size_t accountFailures = 5;
    std::size_t originFailures = 20;
    std::chrono::seconds window = std::chrono::minutes(5);
    std::chrono::seconds blockDuration = std::chrono::minutes(15);
};

/**
 * 单进程限流实现。它像两道门禁：同一账号连续失败会锁账号门，同一来源尝试许多账号会锁
 * 来源门。正式多实例部署时应把相同端口换成共享存储实现，避免攻击者轮询不同进程。
 */
class InMemoryLoginRateLimiter final : public ILoginRateLimiter
{
public:
    explicit InMemoryLoginRateLimiter(LoginRateLimitPolicy policy = {});

    bool allowLogin(std::string_view username, std::string_view origin,
                    TimePoint now) override;
    void recordFailure(std::string_view username, std::string_view origin,
                       TimePoint now) override;
    void recordSuccess(std::string_view username, std::string_view origin,
                       TimePoint now) override;

private:
    struct Bucket
    {
        std::deque<TimePoint> failures;
        std::optional<TimePoint> blockedUntil;
    };

    bool allowed(Bucket &bucket, std::size_t threshold, TimePoint now);
    void addFailure(Bucket &bucket, std::size_t threshold, TimePoint now);
    void prune(Bucket &bucket, TimePoint now);

    LoginRateLimitPolicy policy_;
    std::mutex mutex_;
    std::unordered_map<std::string, Bucket> accounts_;
    std::unordered_map<std::string, Bucket> origins_;
};

} // namespace webserver::phase11
