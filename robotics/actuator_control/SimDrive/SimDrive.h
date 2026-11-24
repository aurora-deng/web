#pragma once

#include "ControlLoop/ControlLoop.h"

namespace nebula::actuator {

// An immediate-response software model, with its own limits and deadline.
class SimDrive {
public:
    static constexpr auto watchdog = std::chrono::milliseconds(80);
    static constexpr Velocity hardLimit{0.4, 1.5};
    void write(Time now, const Output& requested) noexcept;
    Velocity read(Time now) const noexcept;

private:
    Time expires_{};
    Velocity applied_{};
};

}  // namespace nebula::actuator
