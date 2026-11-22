#include "SimNavigation.h"

#include <algorithm>
#include <semaphore>

namespace nebula::fleet {

SimNavigation::SimNavigation(Capabilities stations, std::chrono::milliseconds travelTime)
    : stations_(stations), travelTime_(travelTime) {
    if (!validCapabilities(stations_) || travelTime_.count() < 0 || travelTime_.count() > 30000) {
        throw Error(ErrorCode::invalid, "invalid simulator configuration");
    }
}

NavigationResult SimNavigation::navigate(Station station, std::stop_token stop,
                                        std::chrono::steady_clock::time_point deadline) {
    if (!hasStation(stations_, station)) return NavigationResult::rejected;
    using Clock = std::chrono::steady_clock;
    const auto arrives = Clock::now() + travelTime_;
    const auto until = std::min(arrives, deadline);
    std::binary_semaphore cancelled(0);
    std::stop_callback onStop(stop, [&] { cancelled.release(); });
    while (!stop.stop_requested() && Clock::now() < until) cancelled.try_acquire_until(until);
    return stop.stop_requested() || arrives >= deadline ? NavigationResult::stopped : NavigationResult::arrived;
}
}
