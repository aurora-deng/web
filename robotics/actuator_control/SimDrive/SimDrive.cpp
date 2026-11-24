#include "SimDrive.h"

#include <algorithm>
#include <cmath>

namespace nebula::actuator {

void SimDrive::write(Time now, const Output& requested) noexcept {
    if (!requested.enabled || requested.expires <= now || !std::isfinite(requested.target.linear) ||
        !std::isfinite(requested.target.angular)) {
        expires_ = now;
        applied_ = {};
        return;
    }
    applied_ = {
        std::clamp(requested.target.linear, -hardLimit.linear, hardLimit.linear),
        std::clamp(requested.target.angular, -hardLimit.angular, hardLimit.angular)
    };
    expires_ = std::min(requested.expires, now + watchdog);
}

Velocity SimDrive::read(Time now) const noexcept {
    return now < expires_ ? applied_ : Velocity{};
}

}  // namespace nebula::actuator
