#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace nebula::actuator {

// All stamps use the local monotonic clock. None are sent across machines.
using Time = std::chrono::nanoseconds;

struct Velocity {
    double linear{};   // m/s
    double angular{};  // rad/s
};

struct Motion {
    std::uint64_t sequence{};
    Time received{};
    Velocity target{};
};

struct Permit {
    Time received{};
    bool enabled{};
};

struct Feedback {
    Time received{};
    Velocity measured{};
    bool healthy{};
};

struct Inputs {
    std::optional<Permit> permit;
    std::optional<Feedback> feedback;
    bool emergencyStop{};
};

enum class State { Disarmed, Armed, Fault };
enum class Stop {
    Disarmed, AwaitingCommand, None, PermitRevoked, PermitTimeout,
    CommandTimeout, InvalidCommand, FeedbackTimeout, DriveFault, Emergency
};

struct Output {
    Velocity target{};
    Time expires{};
    Stop reason{Stop::Disarmed};
    bool enabled{};
};

struct Policy {
    Time period = std::chrono::milliseconds(10);
    Time commandAge = std::chrono::milliseconds(150);
    Time permitAge = std::chrono::milliseconds(300);
    Time feedbackAge = std::chrono::milliseconds(100);
    Velocity maxVelocity{0.35, 1.3};
    Velocity maxAcceleration{1.0, 3.5};  // m/s², rad/s²
};

class ControlLoop {
public:
    explicit ControlLoop(Policy policy = {});

    // These entry points all run on the same owner thread.
    bool arm(Time now, const Inputs& input) noexcept;
    bool reset(Time now, const Inputs& input) noexcept;
    void disarm() noexcept;
    Output update(Time now, const Inputs& input,
                  const std::optional<Motion>& command = {}) noexcept;
    State state() const noexcept { return state_; }
    Stop reason() const noexcept { return reason_; }

private:
    void stop(State state, Stop reason) noexcept;

    Policy policy_;
    State state_{State::Disarmed};
    Stop reason_{Stop::Disarmed};
    std::uint64_t lastSequence_{};
    Time armedAt_{};
    std::optional<Motion> motion_;
    Velocity lastTarget_{};
};

const char* name(Stop reason) noexcept;

}  // namespace nebula::actuator
