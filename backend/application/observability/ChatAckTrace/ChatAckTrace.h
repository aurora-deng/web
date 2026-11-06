#pragma once

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <syncstream>

namespace webserver::application
{

// Opt-in per-process budget. Records numeric timing and the persisted message ID only.
class ChatAckTrace final
{
    using Clock = std::chrono::steady_clock;

public:
    ChatAckTrace() noexcept
    {
        auto remaining = budget_.load(std::memory_order_relaxed);
        while (remaining != 0)
        {
            if (budget_.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed))
            {
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

    static Clock::time_point committing() noexcept
    {
        if (!current_)
            return {};
        const auto now = Clock::now();
        current_->commitStartUs_ = micros(now - current_->started_);
        return now;
    }

    static void committed(Clock::time_point start) noexcept
    {
        if (current_ && start != Clock::time_point{})
            current_->commitUs_ = micros(Clock::now() - start);
    }

    void ackReady(std::uint64_t messageId) const noexcept
    {
        if (started_ == Clock::time_point{})
            return;
        const auto elapsed = micros(Clock::now() - started_);
        try
        {
            std::osyncstream(std::clog)
                << "NEBULA_CHAT_ACK_TRACE {\"messageId\":" << messageId
                << ",\"poolAcquireUs\":" << poolUs_
                << ",\"commitStartUs\":" << commitStartUs_
                << ",\"commitUs\":" << commitUs_
                << ",\"ackReadyUs\":" << elapsed << "}\n";
        }
        catch (...) {} // Observability cannot affect an already committed send.
    }

private:
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
    std::int64_t poolUs_ = 0;
    std::int64_t commitStartUs_ = -1;
    std::int64_t commitUs_ = -1;
};

} // namespace webserver::application
