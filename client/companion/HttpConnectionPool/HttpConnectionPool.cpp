#include "HttpConnectionPool.h"
#include <boost/beast/core/flat_buffer.hpp>
#include <mutex>
#include <vector>
#include <stdexcept>

namespace nebula::client {
namespace http = boost::beast::http;
using Clock = std::chrono::steady_clock;
struct HttpConnectionPool::Impl {
    struct Connection {
        // io 的生命周期必须比 socket 长，不能沿用请求栈上的 io_context。
        boost::asio::io_context io;
        SChannelStream stream;
        boost::beast::flat_buffer buffer;
        Clock::time_point created{Clock::now()}, returned{created};
        unsigned requests{};
        explicit Connection(const ServerTrust& trust): stream(io, trust) {}
    };
    ServerTrust trust;
    bool enabled;
    mutable std::mutex mutex;
    std::vector<std::unique_ptr<Connection>> idle;
    HttpPoolStatistics stats;
    std::uint64_t epoch{};
    // 私人轮询与普通 API 共用有界池，不随浏览器并发无限增长。
    static constexpr std::size_t maximum = 8;
    Impl(ServerTrust value, bool reuse): trust(std::move(value)), enabled(reuse) {}
    bool fresh(Connection& c, Clock::time_point now) const {
        return c.stream.socket().is_open() && now-c.returned < std::chrono::seconds(10)
            && now-c.created < std::chrono::seconds(60) && c.requests < 64;
    }
    void prune() {
        const auto now=Clock::now();
        std::erase_if(idle,[&](const auto& c){if(fresh(*c,now))return false;++stats.discarded;return true;});
    }
};
HttpConnectionPool::HttpConnectionPool(ServerTrust trust,bool enabled)
    :impl_(std::make_unique<Impl>(std::move(trust),enabled)) {}
HttpConnectionPool::~HttpConnectionPool()=default;
void HttpConnectionPool::invalidate(std::uint64_t epoch) {
    auto& p=*impl_;std::lock_guard lock(p.mutex);
    p.epoch=epoch;p.stats.discarded+=p.idle.size();p.idle.clear();
}
HttpPoolStatistics HttpConnectionPool::statistics() const {
    auto& p=*impl_;std::lock_guard lock(p.mutex);p.prune();auto result=p.stats;result.idle=p.idle.size();return result;
}
http::response<http::string_body> HttpConnectionPool::request(
    http::request<http::string_body> request,std::uint64_t epoch) {
    auto& p=*impl_;std::unique_ptr<Impl::Connection> connection;bool reused=false;
    {
        std::lock_guard lock(p.mutex);p.prune();
        if(p.stats.active>=Impl::maximum)throw std::runtime_error("Upstream HTTP connection budget exhausted");
        ++p.stats.active;
        if(p.enabled&&epoch==p.epoch&&!p.idle.empty()){
            connection=std::move(p.idle.back());p.idle.pop_back();reused=true;++p.stats.reused;
        }else if(p.stats.active+p.idle.size()>Impl::maximum){++p.stats.discarded;p.idle.pop_back();}
    }
    // 计数无论解析、TLS、内存分配在哪一处失败都必须归还。
    struct ActiveGuard { Impl& p;~ActiveGuard(){std::lock_guard lock(p.mutex);--p.stats.active;} } guard{p};
    const bool safeRead=request.method()==http::verb::get||request.method()==http::verb::head;
    request.keep_alive(p.enabled);
    for(unsigned attempt=0;;++attempt) {
        try {
            if(!connection){
                connection=std::make_unique<Impl::Connection>(p.trust);connection->stream.connect();
                std::lock_guard lock(p.mutex);++p.stats.connected;
            }
            http::write(connection->stream,request);
            http::response_parser<http::string_body> parser;
            parser.body_limit(2*1024*1024);parser.header_limit(32768);
            if(request.method()==http::verb::head)parser.skip(true);
            http::read(connection->stream,connection->buffer,parser);
            auto response=parser.release();++connection->requests;
            // close/EOF 定界、额外未消费字节、升级和信息响应都不进入空闲池。
            const bool keep=p.enabled&&response.result_int()>=200&&response.result_int()!=101
                &&response.keep_alive()&&!response.need_eof()&&connection->buffer.size()==0;
            std::lock_guard lock(p.mutex);
            if(keep&&epoch==p.epoch&&p.fresh(*connection,Clock::now())){
                connection->returned=Clock::now();p.idle.push_back(std::move(connection));
            }else ++p.stats.discarded;
            return response;
        } catch(...) {
            bool disconnected=false;
            try{throw;}catch(const boost::system::system_error& error){
                const auto ec=error.code();disconnected=ec==boost::asio::error::eof||ec==boost::asio::error::connection_reset
                    ||ec==boost::asio::error::broken_pipe||ec==boost::asio::error::connection_aborted||ec==http::error::end_of_stream;
            }catch(...){}
            connection.reset();
            {std::lock_guard lock(p.mutex);++p.stats.discarded;++p.stats.failures;}
            // 空闲连接可能被对端抢先回收；仅幂等读取允许一次新连接重试。
            // POST/PATCH/DELETE 可能已经提交，必须把不确定结果交给上层幂等流程。
            if(!reused||!safeRead||!disconnected||attempt)throw;
            std::lock_guard lock(p.mutex);++p.stats.readRetries;
        }
    }
}
}
