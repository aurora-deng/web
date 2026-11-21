#pragma once
#include "backend/application/observability/Telemetry/Telemetry.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string_view>
#include <syncstream>

namespace webserver::application
{

// Opt-in per-process budget. Records IDs and numeric timings for correlation.
class ChatAckTrace final
{
    using Clock = std::chrono::steady_clock;

public:
    ChatAckTrace() noexcept
    {
        queueUs_ = telemetry::QueueWait::current();
        if (telemetry::metricsEnabled())
            started_ = Clock::now();
        auto remaining = budget_.load(std::memory_order_relaxed);
        while (remaining != 0)
        {
            if (budget_.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed))
            {
                sampled_ = true;
                if (started_ == Clock::time_point{})
                    started_ = Clock::now();
                break;
            }
        }
    }

    class DatabaseScope final
    {
    public:
        explicit DatabaseScope(ChatAckTrace &trace) noexcept : previous_(current_)
        {
            current_ = trace.started_ == Clock::time_point{} ? nullptr : &trace;
        }
        ~DatabaseScope() { current_ = previous_; }
        DatabaseScope(const DatabaseScope &) = delete;
        DatabaseScope &operator=(const DatabaseScope &) = delete;

    private:
        ChatAckTrace *previous_;
    };

    static void acquired(Clock::time_point started) noexcept
    {
        if (current_)
            current_->poolUs_ += micros(Clock::now() - started);
    }

    static Clock::time_point committing(auto backendPid) noexcept
    {
        if (!current_)
            return {};
        const auto now = Clock::now();
        current_->commitStartUs_ = micros(now - current_->started_);
        if (current_->sampled_)
        {
            current_->commitBackendPid_ = backendPid();
            current_->commitStartedMonoNs_ = monotonicNs();
        }
        return now;
    }

    static void committed(Clock::time_point start) noexcept
    {
        if (current_ && start != Clock::time_point{})
        {
            current_->commitUs_ = micros(Clock::now() - start);
            if (current_->sampled_)
                current_->commitFinishedMonoNs_ = monotonicNs();
        }
    }

    void ackReady(std::uint64_t messageId) const noexcept
    {
        if (started_ == Clock::time_point{})
            return;
        const auto elapsed = micros(Clock::now() - started_);
        telemetry::observe(telemetry::Series::Queue, queueUs_);
        telemetry::observe(telemetry::Series::Pool, poolUs_);
        telemetry::observe(telemetry::Series::BeforeCommit, commitStartUs_);
        telemetry::observe(telemetry::Series::Commit, commitUs_);
        telemetry::observe(telemetry::Series::AckReady, elapsed);
        if (!sampled_)
            return;
        try
        {
            std::osyncstream(std::clog)
                << "NEBULA_CHAT_ACK_TRACE {\"messageId\":" << messageId
                << ",\"queueUs\":" << queueUs_
                << ",\"poolAcquireUs\":" << poolUs_
                << ",\"commitStartUs\":" << commitStartUs_
                << ",\"commitUs\":" << commitUs_
                << ",\"commitBackendPid\":" << commitBackendPid_
                << ",\"commitStartedMonoNs\":" << commitStartedMonoNs_
                << ",\"commitFinishedMonoNs\":" << commitFinishedMonoNs_
                << ",\"ackReadyUs\":" << elapsed << "}\n";
        }
        catch (...) {} // Observability cannot affect an already committed send.
    }

private:
    static std::int64_t monotonicNs() noexcept
    {
        // The Linux sampler reads this same clock. It can join observations
        // to a particular COMMIT even if the wall clock steps.
#ifdef __linux__
        timespec value{};
        if (::clock_gettime(CLOCK_MONOTONIC, &value) == 0)
            return static_cast<std::int64_t>(value.tv_sec) * 1'000'000'000 + value.tv_nsec;
#endif
        return -1;
    }

    static std::int64_t micros(Clock::duration duration) noexcept
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    }

    static unsigned sampleLimit() noexcept
    {
        const char *raw = std::getenv("WEB_CHAT_ACK_TRACE_LIMIT");
        if (!raw)
            return 0;
        const std::string_view text(raw);
        unsigned limit = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), limit);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            return 0;
        return limit <= 10000 ? limit : 10000;
    }

    inline static thread_local ChatAckTrace *current_ = nullptr;
    inline static std::atomic<unsigned> budget_{sampleLimit()};

    Clock::time_point started_{};
    bool sampled_ = false;
    std::int64_t queueUs_ = -1;
    std::int64_t poolUs_ = 0;
    std::int64_t commitStartUs_ = -1;
    std::int64_t commitUs_ = -1;
    int commitBackendPid_ = 0;
    std::int64_t commitStartedMonoNs_ = -1;
    std::int64_t commitFinishedMonoNs_ = -1;
};

} // namespace webserver::application
