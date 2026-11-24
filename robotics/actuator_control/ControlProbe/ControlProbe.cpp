#include "ControlLoop/ControlLoop.h"
#include "PeriodicClock/PeriodicClock.h"
#include "SimDrive/SimDrive.h"
#include "SpscQueue/SpscQueue.h"

#include <charconv>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace nebula::actuator;
using namespace std::chrono_literals;

struct Sample {
    Time release;
    CycleTiming cycle;
    Time interval;
    Time read;
    Time update;
    Time write;
    Stop reason;
};

int run(int seconds, const char* path) {
    const Policy policy;
    ControlLoop control(policy);
    SimDrive drive;
    Inputs input;
    SpscQueue<Motion, 8> commands;
    std::vector<Sample> samples;
    samples.reserve(static_cast<std::size_t>(seconds) * 100);
    std::ofstream csv(path);
    csv.exceptions(std::ios::failbit | std::ios::badbit);

    const auto start = monotonicNow() + 250ms;
    std::cout << "ready " << start.count() << std::endl;
    const auto end = start + std::chrono::seconds(seconds);
    FixedSchedule schedule(start, policy.period);
    std::uint64_t sequence = 0;
    Time previousWake{};
    while (schedule.nextRelease() < end) {
        const auto release = schedule.nextRelease();
        sleepUntil(release);
        const auto wake = monotonicNow();

        input.feedback = Feedback{wake, drive.read(wake), true};
        input.permit = Permit{wake, true};
        const auto read = monotonicNow();
        if (sequence == 0 && !control.arm(wake - 1ns,
                {Permit{wake - 1ns, true}, Feedback{wake - 1ns, {}, true}, false})) {
            throw std::runtime_error("simulation could not arm");
        }
        const Velocity request = (sequence / 50) % 2 == 0 ? Velocity{0.21, 0.39} : Velocity{-0.2, -0.27};
        commands.tryPush(Motion{++sequence, wake, request});
        const auto output = control.update(read, input, commands.takeLatest());
        const auto updated = monotonicNow();
        drive.write(updated, output);
        const auto finished = monotonicNow();

        auto cycle = schedule.complete(wake, finished);
        const auto remainingSlots = (end - release - 1ns) / policy.period;
        if (cycle.skipped > static_cast<std::uint64_t>(remainingSlots)) {
            cycle.skipped = static_cast<std::uint64_t>(remainingSlots);
        }
        samples.push_back({release - start, cycle,
            previousWake == Time::zero() ? Time::zero() : wake - previousWake,
            read - wake, updated - read, finished - updated, output.reason});
        previousWake = wake;
    }

    control.disarm();
    const auto stopped = monotonicNow();
    drive.write(stopped, control.update(stopped, input));
    csv << "releaseNs,wakeLateNs,executeNs,finishNs,intervalNs,readNs,updateNs,writeNs,missed,skipped,reason\n";
    for (const auto& s : samples) {
        csv << s.release.count() << ',' << s.cycle.wakeLateness.count() << ','
            << s.cycle.execution.count() << ',' << s.cycle.finishFromRelease.count() << ','
            << s.interval.count() << ',' << s.read.count() << ',' << s.update.count() << ','
            << s.write.count() << ',' << s.cycle.missedDeadline << ',' << s.cycle.skipped << ','
            << name(s.reason) << '\n';
    }
    csv.close();
    std::cout << "cycles " << samples.size() << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: nebula_control_probe SECONDS OUTPUT.csv\n";
        return 2;
    }
    const std::string_view argument(argv[1]);
    int seconds = 0;
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() ||
        seconds < 1 || seconds > 1800) {
        std::cerr << "SECONDS must be an integer from 1 to 1800\n";
        return 2;
    }
    try {
        return run(seconds, argv[2]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
