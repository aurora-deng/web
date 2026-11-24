#pragma once

#include "../../domain/FleetTypes/FleetTypes.h"

#include <chrono>
#include <mutex>
#include <stop_token>

namespace nebula::fleet {

enum class NavigationResult { arrived, rejected, stopped, uncertain };

class NavigationSkill {
public:
    virtual ~NavigationSkill() = default;

    virtual Capabilities stations() const noexcept = 0;
    virtual bool ready() = 0;
    // A validated coordinator exchange renews local authority to navigate.
    virtual void renewCoordinatorLease() noexcept {}
    virtual NavigationResult navigate(const Command& command, std::stop_token stop,
                                      std::chrono::steady_clock::time_point deadline) = 0;
    std::unique_lock<std::mutex> claimChassis() { return std::unique_lock(chassis_, std::try_to_lock); }

private:
    std::mutex chassis_;
};
}
