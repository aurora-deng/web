#pragma once
#include "backend/application/modules/cache/ports/IQueryCache/IQueryCache.h"
#include <memory>
namespace webserver::function3 {
struct RedisCacheConfig {
    std::string address{"127.0.0.1"};
    unsigned port{6379};
    std::string prefix{"nebula:function3:"};
    std::string password;
    unsigned timeoutMs{20};
};
// 有界 RESP2 读缓存适配器：固定只读 EVAL + SETEX/INCR，不执行用户提交的命令。
// I/O 发生在业务 Worker；4 个可复用连接，繁忙或故障时直接回落到 PostgreSQL。
class RedisReadCache final : public IQueryCache {
public:
    explicit RedisReadCache(RedisCacheConfig config);
    ~RedisReadCache();
    CacheLookup lookup(std::string_view bucket) noexcept override;
    void store(std::string_view bucket, std::string_view generation,
               std::string_view value, unsigned seconds) noexcept override;
    void invalidate(std::string_view bucket) noexcept override;
    CacheMetrics metrics() const noexcept override;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
}
