#pragma once

#include "ControlLoop/ControlLoop.h"

namespace nebula::actuator {

Time monotonicNow() noexcept;
void sleepUntil(Time deadline);

struct CycleTiming {
    Time wakeLateness{};
    Time execution{};
    Time finishFromRelease{};
    std::uint64_t skipped{};
    bool missedDeadline{};
};

class FixedSchedule {
public:
    FixedSchedule(Time firstRelease, Time period);
    Time nextRelease() const noexcept { return next_; }
    CycleTiming complete(Time wake, Time finished) noexcept;

private:
    Time next_;
    Time period_;
};

}  // namespace nebula::actuator
