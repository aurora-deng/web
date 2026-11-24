#pragma once

#include "ControlLoop/ControlLoop.h"
#include "PeriodicClock/PeriodicClock.h"
#include "SpscQueue/SpscQueue.h"

#include <thread>

namespace nebula::actuator {

struct Grant {
    Time issued{};  // Zero is an explicit revocation; otherwise unique to a goal.
    Time sampled{};
    Time leaseUntil{};
};

struct Observation {
    State state{};
    Output output{};
    CycleTiming timing{};
    std::uint64_t deadlineMisses{};
    std::uint64_t skipped{};
    std::uint64_t watchdogTrips{};
    Time feedbackAge{};
};

// A single ROS executor thread calls the producer and observe methods.
// The periodic thread owns all control state and performs no ROS operations.
class ActuatorRuntime {
public:
    ActuatorRuntime() = default;
    ~ActuatorRuntime();
    void start();
    void motion(Time now, Velocity command) noexcept;
    void grant(Grant permit) noexcept;
    void feedback(Feedback feedback) noexcept;
    void emergency(bool held) noexcept;
    void requestReset() noexcept;
    std::optional<Observation> observe() noexcept { return output_.takeLatest(); }
    std::uint64_t drops() const noexcept { return drops_.load(std::memory_order_relaxed); }

private:
    void run();

    SpscQueue<Motion, 8> motions_;
    SpscQueue<Grant, 8> grants_;
    SpscQueue<Feedback, 8> feedback_;
    SpscQueue<Observation, 8> output_;
    std::atomic<Time::rep> revokedThrough_{0};
    std::atomic_uint64_t drops_{0};
    std::atomic_bool held_{false};
    std::atomic_bool emergencyPending_{false};
    std::atomic_bool resetRequested_{false};
    std::atomic_bool finished_{false};
    std::uint64_t commandSequence_{};
    Time lastGrantSample_{};
    std::thread worker_;
};

}  // namespace nebula::actuator
