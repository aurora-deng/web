#pragma once
#include <chrono>
#include <optional>

namespace webserver::application {
/** One coalesced commit-notification clock, guarded by the pump's existing mutex.
 * No event map or unbounded tracing buffer. This measures scheduling a batch,
 * not the wire timestamp of an individual message. Idle polling has no sample. */
class CommitWakeClock final {
public:
    using Clock=std::chrono::steady_clock;
    void committed(Clock::time_point point=Clock::now()) noexcept {
        if(!first_)first_=point;
    }
    std::optional<std::chrono::microseconds> dispatchStarted(
        Clock::time_point point=Clock::now()) noexcept {
        if(!first_)return std::nullopt;
        const auto elapsed=point>*first_?point-*first_:Clock::duration::zero();
        first_.reset();
        return std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
    }
private:
    std::optional<Clock::time_point>first_;
};
}
