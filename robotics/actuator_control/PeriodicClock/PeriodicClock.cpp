#include "PeriodicClock.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <thread>

#ifdef __linux__
#include <cerrno>
#include <ctime>
#include <system_error>
#endif

namespace nebula::actuator {

Time monotonicNow() noexcept {
#ifdef __linux__
    timespec stamp{};
    if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) {
        std::abort();
    }
    return std::chrono::seconds(stamp.tv_sec) + Time(stamp.tv_nsec);
#else
    return std::chrono::duration_cast<Time>(std::chrono::steady_clock::now().time_since_epoch());
#endif
}

void sleepUntil(Time deadline) {
#ifdef __linux__
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(deadline);
    timespec stamp{};
    stamp.tv_sec = static_cast<decltype(stamp.tv_sec)>(seconds.count());
    stamp.tv_nsec = static_cast<decltype(stamp.tv_nsec)>((deadline - seconds).count());
    int result;
    do {
        result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &stamp, nullptr);
    } while (result == EINTR);
    if (result != 0) {
        throw std::system_error(result, std::generic_category(), "clock_nanosleep");
    }
#else
    std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(deadline)));
#endif
}

FixedSchedule::FixedSchedule(Time firstRelease, Time period) : next_(firstRelease), period_(period) {
    if (period <= Time::zero()) {
        throw std::invalid_argument("invalid actuator period");
    }
}

CycleTiming FixedSchedule::complete(Time wake, Time finished) noexcept {
    CycleTiming timing{std::max(Time::zero(), wake - next_), finished - wake, finished - next_};
    next_ += period_;
    timing.missedDeadline = finished > next_;
    if (finished >= next_) {
        const auto skipped = (finished - next_) / period_ + 1;
        timing.skipped = static_cast<std::uint64_t>(skipped);
        next_ += period_ * skipped;
    }
    return timing;
}

}  // namespace nebula::actuator
