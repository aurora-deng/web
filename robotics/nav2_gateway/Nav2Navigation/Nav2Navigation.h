#pragma once

#include "backend/fleet/ports/NavigationSkill/NavigationSkill.h"

#include <memory>

namespace nebula::fleet {

// Simulation adapter to a separately launched, namespaced Nav2 stack.
class Nav2Navigation final : public NavigationSkill {
public:
    Nav2Navigation(std::string robotNamespace, Capabilities stations, bool autostart = true);
    ~Nav2Navigation();

    Capabilities stations() const noexcept override { return stations_; }
    bool ready() override;
    void renewCoordinatorLease() noexcept override;
    NavigationResult navigate(const Command& command, std::stop_token stop,
                              std::chrono::steady_clock::time_point deadline) override;

private:
    struct Impl;
    Capabilities stations_;
    std::unique_ptr<Impl> impl_;
};
}
