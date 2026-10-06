#include "server/phase11/security/InMemoryLoginRateLimiter/InMemoryLoginRateLimiter.h"

#include <stdexcept>

namespace webserver::phase11
{

InMemoryLoginRateLimiter::InMemoryLoginRateLimiter(LoginRateLimitPolicy policy)
    : policy_(policy)
{
    if (policy_.accountFailures == 0 || policy_.originFailures == 0 ||
        policy_.window <= std::chrono::seconds::zero() ||
        policy_.blockDuration <= std::chrono::seconds::zero())
        throw std::invalid_argument("invalid login rate-limit policy");
}

void InMemoryLoginRateLimiter::prune(Bucket &bucket, TimePoint now)
{
    while (!bucket.failures.empty() &&
           now - bucket.failures.front() >= policy_.window)
        bucket.failures.pop_front();
    if (bucket.blockedUntil && now >= *bucket.blockedUntil)
    {
        bucket.blockedUntil.reset();
        bucket.failures.clear();
    }
}

bool InMemoryLoginRateLimiter::allowed(Bucket &bucket,
                                       std::size_t threshold,
                                       TimePoint now)
{
    prune(bucket, now);
    return !bucket.blockedUntil && bucket.failures.size() < threshold;
}

void InMemoryLoginRateLimiter::addFailure(Bucket &bucket,
                                          std::size_t threshold,
                                          TimePoint now)
{
    prune(bucket, now);
    bucket.failures.push_back(now);
    if (bucket.failures.size() >= threshold)
        bucket.blockedUntil = now + policy_.blockDuration;
}

bool InMemoryLoginRateLimiter::allowLogin(std::string_view username,
                                          std::string_view origin,
                                          TimePoint now)
{
    std::lock_guard lock(mutex_);
    if (!allowed(accounts_[std::string(username)],
                 policy_.accountFailures, now))
        return false;
    return origin.empty() ||
           allowed(origins_[std::string(origin)], policy_.originFailures, now);
}

void InMemoryLoginRateLimiter::recordFailure(std::string_view username,
                                             std::string_view origin,
                                             TimePoint now)
{
    std::lock_guard lock(mutex_);
    addFailure(accounts_[std::string(username)], policy_.accountFailures, now);
    if (!origin.empty())
        addFailure(origins_[std::string(origin)], policy_.originFailures, now);
}

void InMemoryLoginRateLimiter::recordSuccess(std::string_view username,
                                             std::string_view,
                                             TimePoint)
{
    std::lock_guard lock(mutex_);
    // 成功登录证明账号凭证正确，可以清空账号桶；来源桶保留，避免攻击者用一个已知账号
    // 穿插成功登录来洗掉对其他账号的枚举记录。
    accounts_.erase(std::string(username));
}

} // namespace webserver::phase11
