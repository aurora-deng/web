#pragma once
#include "backend/application/observability/tracing/Telemetry.h"
#include <atomic>

namespace webserver::telemetry {
// A fixed-name asynchronous span; it never installs or restores thread-local state.
class DetachedSpan final {
public:
    explicit DetachedSpan(Series series) noexcept;
    ~DetachedSpan();
    void finish(int status) noexcept;
    DetachedSpan(const DetachedSpan &)=delete;
    DetachedSpan &operator=(const DetachedSpan &)=delete;
private:
    std::atomic_bool finished_{true};
    std::string trace_,parent_,id_;
    std::string_view name_;
    Clock::time_point started_{};
    std::int64_t unixStart_{};
};
}
