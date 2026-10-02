#pragma once

#include <cstdint>
#include <sstream>
#include <string>

#include "server/ops/StripedCounter.h"

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
    struct Snapshot
    {
        std::uint64_t httpRequests{0};
        std::uint64_t authenticationRejected{0};
        std::uint64_t originRejected{0};
        std::uint64_t rateLimited{0};
        std::uint64_t grpcStarted{0};
        std::uint64_t grpcCompleted{0};
        std::uint64_t grpcCancelled{0};
        std::uint64_t grpcRejected{0};
    };

    void recordHttpRequest() noexcept { httpRequests_.add(); }
    void recordAuthenticationRejected() noexcept { authenticationRejected_.add(); }
    void recordOriginRejected() noexcept { originRejected_.add(); }
    void recordRateLimited() noexcept { rateLimited_.add(); }
    void recordGrpcStarted() noexcept { grpcStarted_.add(); }
    void recordGrpcCompleted() noexcept { grpcCompleted_.add(); }
    void recordGrpcCancelled() noexcept { grpcCancelled_.add(); }
    void recordGrpcRejected() noexcept { grpcRejected_.add(); }

    [[nodiscard]] Snapshot snapshot() const noexcept
    {
        return {
            httpRequests_.load(),
            authenticationRejected_.load(),
            originRejected_.load(),
            rateLimited_.load(),
            grpcStarted_.load(),
            grpcCompleted_.load(),
            grpcCancelled_.load(),
            grpcRejected_.load()};
    }

    [[nodiscard]] std::string prometheus(
        std::size_t webSocketOnline,
        std::size_t sseOnline,
        std::size_t sseHistoryEvents) const
    {
        const auto values = snapshot();
        std::ostringstream out;
        out << "# TYPE webserver_http_requests_total counter\n"
            << "webserver_http_requests_total " << values.httpRequests << '\n'
            << "# TYPE webserver_auth_rejected_total counter\n"
            << "webserver_auth_rejected_total " << values.authenticationRejected << '\n'
            << "# TYPE webserver_origin_rejected_total counter\n"
            << "webserver_origin_rejected_total " << values.originRejected << '\n'
            << "# TYPE webserver_rate_limited_total counter\n"
            << "webserver_rate_limited_total " << values.rateLimited << '\n'
            << "# TYPE webserver_grpc_started_total counter\n"
            << "webserver_grpc_started_total " << values.grpcStarted << '\n'
            << "webserver_grpc_completed_total " << values.grpcCompleted << '\n'
            << "webserver_grpc_cancelled_total " << values.grpcCancelled << '\n'
            << "webserver_grpc_rejected_total " << values.grpcRejected << '\n'
            << "# TYPE webserver_websocket_online gauge\n"
            << "webserver_websocket_online " << webSocketOnline << '\n'
            << "# TYPE webserver_sse_online gauge\n"
            << "webserver_sse_online " << sseOnline << '\n'
            << "# TYPE webserver_sse_history_events gauge\n"
            << "webserver_sse_history_events " << sseHistoryEvents << '\n';
        return out.str();
    }

private:
    StripedCounter httpRequests_;
    StripedCounter authenticationRejected_;
    StripedCounter originRejected_;
    StripedCounter rateLimited_;
    StripedCounter grpcStarted_;
    StripedCounter grpcCompleted_;
    StripedCounter grpcCancelled_;
    StripedCounter grpcRejected_;
};

} // namespace webserver::ops
