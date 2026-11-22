#pragma once

#include "../../ports/NavigationSkill/NavigationSkill.h"

namespace nebula::fleet {

class SimNavigation final : public NavigationSkill {
public:
    SimNavigation(Capabilities stations, std::chrono::milliseconds travelTime);
    Capabilities stations() const noexcept override { return stations_; }
    bool ready() override { return true; }
    NavigationResult navigate(Station station, std::stop_token stop,
                              std::chrono::steady_clock::time_point deadline) override;

private:
    Capabilities stations_;
    std::chrono::milliseconds travelTime_;
};
}
