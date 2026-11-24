#include "ControlLoop/ControlLoop.h"
#include "PeriodicClock/PeriodicClock.h"
#include "SimDrive/SimDrive.h"
#include "SpscQueue/SpscQueue.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {

using namespace nebula::actuator;
using namespace std::chrono_literals;

void expect(bool condition, const char* detail) {
    if (!condition) {
        throw std::runtime_error(detail);
    }
}

Inputs healthy(Time now) {
    return {Permit{now, true}, Feedback{now, {}, true}, false};
}

void stopped(Output out, Stop reason) {
    expect(!out.enabled && out.target.linear == 0 && out.target.angular == 0 && out.reason == reason,
           name(reason));
}

void motionLease() {
    ControlLoop loop;
    stopped(loop.update(0ms, {}), Stop::Disarmed);
    expect(!loop.arm(0ms, {}), "permission and feedback are required");
    expect(loop.arm(10ms, healthy(10ms)), "arm accepted");
    stopped(loop.update(11ms, healthy(11ms), Motion{1, 9ms, {10, 25}}), Stop::AwaitingCommand);
    auto out = loop.update(20ms, healthy(20ms), Motion{2, 19ms, {10, -25}});
    expect(out.enabled && out.target.linear == 0.01 && std::abs(out.target.angular + 0.035) < 1e-9,
           "first tick obeys ramp");
    for (int i = 30; i <= 450; i += 10) {
        const auto now = std::chrono::milliseconds(i);
        out = loop.update(now, healthy(now), Motion{static_cast<std::uint64_t>(i), now, {10, -25}});
    }
    expect(std::abs(out.target.linear - 0.35) < 1e-9 && std::abs(out.target.angular + 1.3) < 1e-9,
           "L1 hard bounds");
    auto revoke = healthy(460ms);
    revoke.permit->enabled = false;
    stopped(loop.update(460ms, revoke), Stop::PermitRevoked);
    expect(loop.arm(500ms, healthy(500ms)), "fresh permit can rearm after voluntary stop");
    stopped(loop.update(510ms, healthy(510ms), Motion{450, 509ms, {1, 1}}), Stop::AwaitingCommand);
    out = loop.update(520ms, healthy(520ms), Motion{451, 519ms, {1, 1}});
    expect(out.enabled && out.expires == 620ms, "earliest expiry is carried to drive");
    out = loop.update(550ms, healthy(550ms), Motion{449, 549ms, {-1, -1}});
    expect(out.enabled && out.target.linear > 0, "out-of-order command cannot change direction");
    stopped(loop.update(669ms, healthy(669ms), Motion{451, 669ms, {1, 1}}), Stop::CommandTimeout);
}

void stickyFaults() {
    ControlLoop loop;
    auto in = healthy(0ms);
    expect(loop.arm(0ms, in), "initial arm");
    in.feedback = Feedback{40ms, {}, false};
    in.permit = Permit{40ms, true};
    stopped(loop.update(40ms, in), Stop::DriveFault);
    expect(!loop.reset(40ms, in), "faulty feedback blocks reset");
    in = healthy(41ms);
    in.feedback->measured.linear = 0.01;
    expect(!loop.reset(41ms, in), "moving feedback blocks reset");
    expect(!loop.arm(42ms, healthy(42ms)), "fresh inputs cannot clear a latch");
    expect(loop.reset(42ms, healthy(42ms)), "explicit reset returns disarmed");
    expect(loop.state() == State::Disarmed, "reset has no arm side effect");
    expect(loop.arm(50ms, healthy(50ms)), "manual re-arm accepted");
    in = healthy(51ms);
    in.emergencyStop = true;
    stopped(loop.update(51ms, in, Motion{10, 51ms, {0.3, 0.5}}), Stop::Emergency);
    expect(!loop.reset(51ms, in), "held emergency stop blocks reset");
    loop.disarm();
    stopped(loop.update(52ms, healthy(52ms)), Stop::Emergency);
    expect(loop.reset(52ms, healthy(52ms)), "reset after emergency is released");
    expect(loop.arm(52ms, healthy(52ms)), "explicit re-arm accepted");
    stopped(loop.update(61ms, healthy(61ms), Motion{10, 60ms, {0.3, 0.5}}), Stop::AwaitingCommand);
    stopped(loop.update(62ms, healthy(62ms),
                        Motion{11, 62ms, {std::numeric_limits<double>::quiet_NaN(), 0}}),
            Stop::InvalidCommand);
}

void inputDeadlines() {
    ControlLoop loop;
    auto input = healthy(100ms);
    expect(loop.arm(100ms, input), "initial arm");
    expect(loop.update(101ms, healthy(101ms), Motion{1, 101ms, {0.2, 0.1}}).enabled, "moving");
    stopped(loop.update(200ms, input), Stop::FeedbackTimeout);
    expect(loop.reset(220ms, healthy(220ms)), "reset after feedback returns");
    input = healthy(220ms);
    expect(loop.arm(220ms, input), "arm after recovery");
    for (int elapsed = 10; elapsed <= 290; elapsed += 10) {
        const auto now = std::chrono::milliseconds(220 + elapsed);
        input.feedback = healthy(now).feedback;
        expect(loop.update(now, input, Motion{static_cast<std::uint64_t>(elapsed), now, {0.2, 0}}).enabled,
               "fresh feedback and command are accepted before permit deadline");
    }
    input.feedback = healthy(520ms).feedback;
    stopped(loop.update(520ms, input, Motion{300, 520ms, {0.2, 0}}), Stop::PermitTimeout);
    stopped(loop.update(530ms, healthy(530ms)), Stop::PermitTimeout);
}

void independentDriveLimits() {
    SimDrive drive;
    expect(drive.read(10ms).linear == 0, "drive starts neutral");
    drive.write(10ms, {{400, -600}, 1000ms, Stop::None, true});
    expect(drive.read(11ms).linear == 0.4 && drive.read(11ms).angular == -1.5, "drive clamps bypassed L1");
    expect(drive.read(89ms).linear == 0.4 && drive.read(90ms).linear == 0, "independent drive watchdog");
    drive.write(100ms, {{0.2, 0.3}, 103ms, Stop::None, true});
    expect(drive.read(103ms).linear == 0, "command expiry is never extended by drive");
    drive.write(110ms, {{0.2, 0.3}, 109ms, Stop::None, true});
    expect(drive.read(110ms).linear == 0, "late write stays neutral");
}

void periodicSlots() {
    FixedSchedule schedule(100ms, 10ms);
    auto result = schedule.complete(101ms, 102ms);
    expect(result.wakeLateness == 1ms && result.execution == 1ms && result.finishFromRelease == 2ms &&
           result.skipped == 0 && !result.missedDeadline && schedule.nextRelease() == 110ms, "normal cycle");
    result = schedule.complete(134ms, 136ms);
    expect(result.wakeLateness == 24ms && result.skipped == 2 && result.missedDeadline &&
           schedule.nextRelease() == 140ms, "skip crossed releases without burst replay");
    result = schedule.complete(141ms, 150ms);
    expect(!result.missedDeadline && result.skipped == 1 && schedule.nextRelease() == 160ms,
           "finishing exactly on next slot does not replay that slot");
}

void handoff() {
    SpscQueue<Motion, 3> queue;
    expect(!queue.takeLatest(), "initially empty");
    expect(queue.tryPush({1}) && queue.tryPush({2}) && queue.tryPush({3}) && !queue.tryPush({4}),
           "capacity is fixed");
    expect(queue.takeLatest()->sequence == 3 && !queue.takeLatest(), "consumer keeps newest queued sample");
    expect(queue.tryPush({4}) && queue.takeLatest()->sequence == 4, "wraparound");

    SpscQueue<Motion, 64> concurrent;
    constexpr std::uint64_t last = 50'000;
    std::thread producer([&] {
        for (std::uint64_t id = 1; id <= last; ++id) {
            const Motion motion{id, Time(id), {static_cast<double>(id), -static_cast<double>(id)}};
            while (!concurrent.tryPush(motion)) {
                std::this_thread::yield();
            }
        }
    });
    std::uint64_t seen = 0;
    bool intact = true;
    while (seen < last) {
        if (const auto motion = concurrent.takeLatest()) {
            intact = intact && motion->sequence > seen && motion->received == Time(motion->sequence) &&
                     motion->target.linear == static_cast<double>(motion->sequence) &&
                     motion->target.linear == -motion->target.angular;
            seen = motion->sequence;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    expect(intact, "concurrent delivery uses complete, ordered snapshots");
}

}  // namespace

int main() try {
    motionLease();
    stickyFaults();
    inputDeadlines();
    independentDriveLimits();
    periodicSlots();
    handoff();
    std::cout << "6 actuator contract groups passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
