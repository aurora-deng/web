#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
namespace webserver::function3 {
// 缓存只保存可重新计算的读结果；权限、账号和消息持久化仍以数据库为准。
struct CacheLookup { std::optional<std::string> value; std::string generation; };
struct CacheMetrics { std::uint64_t hits{}, misses{}, errors{}, bypassed{}; };
class IQueryCache {
public:
    virtual ~IQueryCache() = default;
    virtual CacheLookup lookup(std::string_view bucket) noexcept = 0;
    // 必须使用查询前拿到的 generation，避免旧查询在失效后污染新一代缓存。
    virtual void store(std::string_view bucket, std::string_view generation,
                       std::string_view value, unsigned seconds) noexcept = 0;
    virtual void invalidate(std::string_view bucket) noexcept = 0;
    virtual CacheMetrics metrics() const noexcept = 0;
};
}
