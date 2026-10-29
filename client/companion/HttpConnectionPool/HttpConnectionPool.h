#pragma once
#include "client/companion/SChannelStream/SChannelStream.h"
#include <boost/beast/http.hpp>
#include <cstdint>
#include <memory>

namespace nebula::client {
struct HttpPoolStatistics {
    std::uint64_t connected{}, reused{}, discarded{}, readRetries{}, failures{};
    std::size_t active{}, idle{};
};
/** 普通短请求独享一个租约；SSE、WS、下载不进入此池。
 * 连接不保存 Cookie，每个请求仍独立鉴权。失效的写请求绝不自动重发。
 */
class HttpConnectionPool final {
public:
    explicit HttpConnectionPool(ServerTrust trust, bool enabled = true);
    ~HttpConnectionPool();
    HttpConnectionPool(const HttpConnectionPool&) = delete;
    HttpConnectionPool& operator=(const HttpConnectionPool&) = delete;
    boost::beast::http::response<boost::beast::http::string_body> request(
        boost::beast::http::request<boost::beast::http::string_body> request,
        std::uint64_t identityEpoch);
    // 切换/吊销身份时清除空闲连接；已租出的旧连接返回时也不能入池。
    void invalidate(std::uint64_t identityEpoch);
    HttpPoolStatistics statistics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
