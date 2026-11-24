#include "ControlLoop.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace nebula::actuator {
namespace {

bool finite(Velocity value) noexcept {
    return std::isfinite(value.linear) && std::isfinite(value.angular);
}

bool positive(Velocity value) noexcept {
    return finite(value) && value.linear > 0 && value.angular > 0;
}

bool fresh(Time now, Time stamp, Time age) noexcept {
    return stamp <= now && now - stamp < age;
}

double approach(double from, double to, double bound, double step) noexcept {
    return from + std::clamp(std::clamp(to, -bound, bound) - from, -step, step);
}

}  // namespace

ControlLoop::ControlLoop(Policy policy) : policy_(policy) {
    if (policy.period <= Time::zero() || policy.commandAge < policy.period ||
        policy.permitAge < policy.period || policy.feedbackAge < policy.period ||
        !positive(policy.maxVelocity) || !positive(policy.maxAcceleration)) {
        throw std::invalid_argument("invalid actuator control policy");
    }
}

bool ControlLoop::arm(Time now, const Inputs& in) noexcept {
    if (state_ != State::Disarmed || in.emergencyStop || !in.permit || !in.permit->enabled ||
        !fresh(now, in.permit->received, policy_.permitAge) || !in.feedback ||
        !fresh(now, in.feedback->received, policy_.feedbackAge) || !in.feedback->healthy ||
        !finite(in.feedback->measured)) {
        return false;
    }
    armedAt_ = now;
    stop(State::Armed, Stop::AwaitingCommand);
    return true;
}

bool ControlLoop::reset(Time now, const Inputs& in) noexcept {
    if (state_ != State::Fault || in.emergencyStop || !in.feedback ||
        !fresh(now, in.feedback->received, policy_.feedbackAge) || !in.feedback->healthy ||
        !finite(in.feedback->measured) || in.feedback->measured.linear != 0.0 ||
        in.feedback->measured.angular != 0.0) {
        return false;
    }
    stop(State::Disarmed, Stop::Disarmed);
    return true;
}

void ControlLoop::disarm() noexcept {
    if (state_ != State::Fault) {
        stop(State::Disarmed, Stop::Disarmed);
    }
}

void ControlLoop::stop(State state, Stop reason) noexcept {
    state_ = state;
    reason_ = reason;
    motion_.reset();
    lastTarget_ = {};
}

Output ControlLoop::update(Time now, const Inputs& in, const std::optional<Motion>& command) noexcept {
    const bool nextCommand = command && command->sequence > lastSequence_;
    if (nextCommand) {
        lastSequence_ = command->sequence;
    }

    if (in.emergencyStop) {
        stop(State::Fault, Stop::Emergency);
    } else if (state_ == State::Armed) {
        if (!in.feedback || !fresh(now, in.feedback->received, policy_.feedbackAge)) {
            stop(State::Fault, Stop::FeedbackTimeout);
        } else if (!in.feedback->healthy || !finite(in.feedback->measured)) {
            stop(State::Fault, Stop::DriveFault);
        } else if (!in.permit || !fresh(now, in.permit->received, policy_.permitAge)) {
            stop(State::Fault, Stop::PermitTimeout);
        } else if (!in.permit->enabled) {
            stop(State::Disarmed, Stop::PermitRevoked);
        } else if (nextCommand && command->received > armedAt_ &&
                   fresh(now, command->received, policy_.commandAge)) {
            if (finite(command->target)) {
                motion_ = command;
            } else {
                stop(State::Fault, Stop::InvalidCommand);
            }
        }
    }
    if (state_ != State::Armed) {
        return {{}, now, reason_, false};
    }
    if (!fresh(now, motion_ ? motion_->received : armedAt_, policy_.commandAge)) {
        stop(State::Fault, Stop::CommandTimeout);
        return {{}, now, reason_, false};
    }
    if (!motion_) {
        return {{}, now, Stop::AwaitingCommand, false};
    }

    const auto seconds = std::chrono::duration<double>(policy_.period).count();
    lastTarget_.linear = approach(lastTarget_.linear, motion_->target.linear,
                                  policy_.maxVelocity.linear, seconds * policy_.maxAcceleration.linear);
    lastTarget_.angular = approach(lastTarget_.angular, motion_->target.angular,
                                   policy_.maxVelocity.angular, seconds * policy_.maxAcceleration.angular);
    const auto expires = std::min({motion_->received + policy_.commandAge,
                                   in.permit->received + policy_.permitAge,
                                   in.feedback->received + policy_.feedbackAge});
    reason_ = Stop::None;
    return {lastTarget_, expires, reason_, true};
}

const char* name(Stop reason) noexcept {
    switch (reason) {
    case Stop::Disarmed: return "disarmed";
    case Stop::AwaitingCommand: return "awaiting_command";
    case Stop::None: return "ok";
    case Stop::PermitRevoked: return "permit_revoked";
    case Stop::PermitTimeout: return "permit_timeout";
    case Stop::CommandTimeout: return "command_timeout";
    case Stop::InvalidCommand: return "invalid_command";
    case Stop::FeedbackTimeout: return "feedback_timeout";
    case Stop::DriveFault: return "drive_fault";
    case Stop::Emergency: return "emergency_stop";
    }
    return "unknown";
}

}  // namespace nebula::actuator
