#pragma once

#include <atomic>
#include <cstdint>
#include <sstream>
#include <string>

namespace webserver::ops
{

/**
 * @brief Phase 9 的跨协议运行指标。
 *
 * 计数器只用于观测，不参与业务正确性判断，因此使用 relaxed atomic。Prometheus
 * 抓取时允许各字段相差一个并发事件，和汽车仪表盘允许瞬时读数略有先后相同。
 */
class OperationalMetrics
{
public:
    std::atomic<std::uint64_t> httpRequests{0};
    std::atomic<std::uint64_t> authenticationRejected{0};
    std::atomic<std::uint64_t> originRejected{0};
    std::atomic<std::uint64_t> rateLimited{0};
    std::atomic<std::uint64_t> grpcStarted{0};
    std::atomic<std::uint64_t> grpcCompleted{0};
    std::atomic<std::uint64_t> grpcCancelled{0};
    std::atomic<std::uint64_t> grpcRejected{0};

    [[nodiscard]] std::string prometheus(
        std::size_t webSocketOnline,
        std::size_t sseOnline,
        std::size_t sseHistoryEvents) const
    {
        std::ostringstream out;
        out << "# TYPE webserver_http_requests_total counter\n"
            << "webserver_http_requests_total " << load(httpRequests) << '\n'
            << "# TYPE webserver_auth_rejected_total counter\n"
            << "webserver_auth_rejected_total " << load(authenticationRejected) << '\n'
            << "# TYPE webserver_origin_rejected_total counter\n"
            << "webserver_origin_rejected_total " << load(originRejected) << '\n'
            << "# TYPE webserver_rate_limited_total counter\n"
            << "webserver_rate_limited_total " << load(rateLimited) << '\n'
            << "# TYPE webserver_grpc_started_total counter\n"
            << "webserver_grpc_started_total " << load(grpcStarted) << '\n'
            << "webserver_grpc_completed_total " << load(grpcCompleted) << '\n'
            << "webserver_grpc_cancelled_total " << load(grpcCancelled) << '\n'
            << "webserver_grpc_rejected_total " << load(grpcRejected) << '\n'
            << "# TYPE webserver_websocket_online gauge\n"
            << "webserver_websocket_online " << webSocketOnline << '\n'
            << "# TYPE webserver_sse_online gauge\n"
            << "webserver_sse_online " << sseOnline << '\n'
            << "# TYPE webserver_sse_history_events gauge\n"
            << "webserver_sse_history_events " << sseHistoryEvents << '\n';
        return out.str();
    }

private:
    static std::uint64_t load(const std::atomic<std::uint64_t> &value)
    {
        return value.load(std::memory_order_relaxed);
    }
};

} // namespace webserver::ops
