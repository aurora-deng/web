#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace webserver::telemetry
{
using Clock = std::chrono::steady_clock;

enum class Series { Conversations, History, Queue, Pool, BeforeCommit, Commit, AckReady, Count };

bool metricsEnabled() noexcept;
void observe(Series series, std::int64_t microseconds) noexcept;
std::string metrics();
std::int64_t elapsedUs(Clock::time_point start) noexcept;

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
    explicit Span(std::string_view name, std::string_view traceparent = {}, bool server = false) noexcept;
    ~Span();
    Span(const Span &) = delete;
    Span &operator=(const Span &) = delete;
    void error() noexcept { error_ = true; }
private:
    static thread_local Span *current_;
    Span *previous_ = nullptr;
    std::string name_, trace_, id_, parent_;
    Clock::time_point steadyStart_{};
    std::int64_t unixStart_ = 0;
    bool server_ = false;
    bool error_ = false;
};
}
