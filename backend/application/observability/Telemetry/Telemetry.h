#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <source_location>
#include <utility>

namespace webserver::telemetry
{
using Clock = std::chrono::steady_clock;

enum class Series { Conversations, History, Queue, Pool, BeforeCommit, Commit, AckReady,
#define NEBULA_OPERATION(enumKey, key, domain, title, name) enumKey,
#include "backend/application/observability/OperationCatalog/OperationCatalog.def"
#undef NEBULA_OPERATION
                    Count };

bool metricsEnabled() noexcept;
bool sampleChatSend() noexcept;
void observe(Series series, std::int64_t microseconds) noexcept;
void recordHttpResponse(int status) noexcept;
std::string metrics();
std::int64_t elapsedUs(Clock::time_point start) noexcept;
void submitDiagnostic(std::string record) noexcept;
bool tracingEnabled() noexcept;
void submitSpan(std::string record, bool failed) noexcept;
void recordTraceDrop() noexcept;

class Latency final
{
public:
    explicit Latency(Series series) noexcept;
    ~Latency();
private:
    Series series_;
    Clock::time_point started_;
};

// Set only while a worker runs the submitted job. Nested calls see the same wait.
class QueueWait final
{
public:
    explicit QueueWait(std::int64_t microseconds) noexcept;
    ~QueueWait();
    static std::int64_t current() noexcept;
private:
    std::int64_t previous_;
};

class Span final
{
public:
    enum class Kind { Internal = 1, Server = 2, Client = 3 };

    explicit Span(std::string_view name, std::string_view traceparent = {},
                  Kind kind = Kind::Internal, bool sampledRoot = false,
                  std::source_location location = std::source_location::current()) noexcept;
    ~Span();
    Span(const Span &) = delete;
    Span &operator=(const Span &) = delete;
    void error() noexcept { error_ = true; }
    void cancel() noexcept { cancelled_ = true; }
    static std::pair<std::string,std::string> context();
    static std::string traceparent() noexcept;
    std::string_view traceId() const noexcept { return trace_; }
    void httpStatus(int status) noexcept
    {
        httpStatus_ = status;
        if (status >= 400) error();
    }
private:
    static thread_local Span *current_;
    Span *previous_ = nullptr;
    std::string name_, trace_, id_, parent_;
    Clock::time_point steadyStart_{};
    std::int64_t unixStart_ = 0;
    int exceptionsAtStart_ = 0;
    Kind kind_;
    int httpStatus_ = 0;
    bool error_ = false;
    bool cancelled_ = false;
    bool httpFailureOnly_ = false;
    std::source_location location_;
};
}
