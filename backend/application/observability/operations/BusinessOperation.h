#pragma once
#include "backend/application/observability/tracing/Telemetry.h"
#include <atomic>
#include <memory>

namespace webserver::telemetry {
class DetachedSpan;
enum class Outcome { Success, Error, Cancelled, Timeout };
std::string_view operationName(Series series) noexcept;
std::string operationMetrics();

// A single admitted asynchronous operation. No thread-local span is kept alive
// across callbacks. Completion can race cancellation, but records exactly once.
class AsyncBusinessOperation final {
public:
    AsyncBusinessOperation() noexcept;
    ~AsyncBusinessOperation();
    AsyncBusinessOperation(const AsyncBusinessOperation &) = delete;
    AsyncBusinessOperation &operator=(const AsyncBusinessOperation &) = delete;
    void start(Series series,bool traced=false) noexcept; // Before publishing; once per instance.
    void finish(Outcome outcome) noexcept;
private:
    Series series_ = Series::Count;
    Clock::time_point started_{};
    std::atomic_bool finished_{true};
    std::unique_ptr<DetachedSpan> span_;
};

class BusinessOperation final {
public:
    explicit BusinessOperation(Series series,
        std::source_location location = std::source_location::current(), bool background = false,
        std::string_view traceparent={}) noexcept;
    ~BusinessOperation();
    void fail() noexcept { outcome_=Outcome::Error; span_.error(); }
    void cancel() noexcept { outcome_=Outcome::Cancelled; span_.cancel(); }
    BusinessOperation(const BusinessOperation &) = delete;
    BusinessOperation &operator=(const BusinessOperation &) = delete;
private:
    AsyncBusinessOperation measurement_;
    Span span_;
    int exceptions_;
    Outcome outcome_ = Outcome::Success;
};
}
