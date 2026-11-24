#include "ActuatorRuntime.h"

#include "SimDrive/SimDrive.h"

#include <algorithm>

namespace nebula::actuator {

ActuatorRuntime::~ActuatorRuntime() {
    finished_.store(true, std::memory_order_relaxed);
    if (worker_.joinable()) {
        worker_.join();
    }
}

void ActuatorRuntime::start() {
    worker_ = std::thread(&ActuatorRuntime::run, this);
}

void ActuatorRuntime::motion(Time now, Velocity command) noexcept {
    if (!motions_.tryPush({++commandSequence_, now, command})) {
        drops_.fetch_add(1, std::memory_order_relaxed);
    }
}

void ActuatorRuntime::grant(Grant permit) noexcept {
    if (permit.sampled <= lastGrantSample_) {
        return;
    }
    lastGrantSample_ = permit.sampled;
    if (permit.issued == Time::zero()) {
        // A revocation survives queue coalescing and overflow.
        revokedThrough_.store(permit.sampled.count(), std::memory_order_release);
    }
    if (!grants_.tryPush(permit)) {
        drops_.fetch_add(1, std::memory_order_relaxed);
    }
}

void ActuatorRuntime::feedback(Feedback feedback) noexcept {
    if (!feedback_.tryPush(feedback)) {
        drops_.fetch_add(1, std::memory_order_relaxed);
    }
}

void ActuatorRuntime::emergency(bool held) noexcept {
    held_.store(held);
    if (held) {
        emergencyPending_.store(true);
    }
}

void ActuatorRuntime::requestReset() noexcept {
    resetRequested_.store(true, std::memory_order_release);
}

void ActuatorRuntime::run() {
    static_assert(std::atomic<Time::rep>::is_always_lock_free && std::atomic_uint64_t::is_always_lock_free);
    const Policy policy;
    ControlLoop control(policy);
    const auto started = monotonicNow();
    FixedSchedule schedule(started + policy.period, policy.period);
    Inputs input;
    Grant permit;
    // A restarted controller cannot resume a grant issued before it started.
    Time lastArmedGrant = started;
    Time lastWake{};
    std::optional<Motion> latestMotion;
    std::uint64_t misses = 0;
    std::uint64_t skipped = 0;
    std::uint64_t watchdogTrips = 0;

    while (!finished_.load(std::memory_order_relaxed)) {
        sleepUntil(schedule.nextRelease());
        const auto wake = monotonicNow();
        if (lastWake != Time::zero() && wake - lastWake >= SimDrive::watchdog &&
            control.state() == State::Armed) {
            control.disarm();
            lastArmedGrant = wake;
            ++watchdogTrips;
        }
        lastWake = wake;
        if (const auto newGrant = grants_.takeLatest()) {
            permit = *newGrant;
        }
        if (const auto newFeedback = feedback_.takeLatest()) {
            input.feedback = *newFeedback;
        }
        const auto command = motions_.takeLatest();
        if (command) {
            latestMotion = command;
        }
        const auto now = monotonicNow();
        const auto revokedThrough = Time(revokedThrough_.load(std::memory_order_acquire));
        const bool allowed = permit.issued > revokedThrough && now < permit.leaseUntil;
        input.permit = Permit{permit.sampled, allowed};
        const bool emergency = emergencyPending_.exchange(false);
        input.emergencyStop = held_.load() || emergency;

        if (resetRequested_.exchange(false, std::memory_order_acquire) && control.reset(now, input)) {
            lastArmedGrant = now;
        }
        if (allowed && permit.issued > lastArmedGrant) {
            // A new goal never inherits the previous goal's target.
            if (control.state() == State::Armed) {
                control.disarm();
            }
            if (latestMotion && latestMotion->received >= permit.issued &&
                now - latestMotion->received < policy.commandAge && control.arm(now, input)) {
                lastArmedGrant = permit.issued;
            }
        }

        auto target = control.update(now, input, command);
        target.expires = std::min({target.expires, permit.leaseUntil, now + SimDrive::watchdog});
        const auto timing = schedule.complete(wake, monotonicNow());
        misses += timing.missedDeadline;
        skipped += timing.skipped;
        const auto feedbackAge = input.feedback ? now - input.feedback->received : Time(-1);
        if (!output_.tryPush({control.state(), target, timing, misses, skipped, watchdogTrips, feedbackAge})) {
            drops_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

}  // namespace nebula::actuator
