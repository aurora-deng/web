#include "../EdgeSession/EdgeSession.h"
#include "../../../infrastructure/SimNavigation/SimNavigation.h"
#include "../../../infrastructure/SqliteEdgeLedger/SqliteEdgeLedger.h"
#include "backend/workflow/ObserverEvents/ObserverEvents.h"
#include "ProcessStop.h"

#ifdef NEBULA_FLEET_NAV2
#include "robotics/nav2_gateway/Nav2Navigation/Nav2Navigation.h"
#endif

#include <charconv>
#include <fstream>
#include <iostream>
#include <memory>

using namespace nebula::fleet;
using namespace nebula::fleet::wire;
using namespace std::chrono_literals;

namespace {
unsigned number(std::string_view text) {
    unsigned result{};
    const auto last = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), last, result);
    if (parsed.ec != std::errc{} || parsed.ptr != last) throw Error(ErrorCode::invalid, "invalid edge number");
    return result;
}

std::unique_ptr<NavigationSkill> navigationFor(std::string_view adapter, const char* robot,
                                               Capabilities capabilities) {
    if (adapter == "nav2") {
#ifdef NEBULA_FLEET_NAV2
        return std::make_unique<Nav2Navigation>(robot, capabilities);
#else
        throw Error(ErrorCode::invalid, "Nav2 simulation adapter is disabled in this build");
#endif
    }
    return std::make_unique<SimNavigation>(capabilities, std::chrono::milliseconds{number(adapter)});
}

class EventFile {
public:
    explicit EventFile(const char* path) {
        if (!path) return;
        path_ = std::filesystem::path(path);
        if (std::filesystem::exists(path_)) bytes_ = std::filesystem::file_size(path_);
        enabled_ = true;
    }

    void collect(const nebula::workflow::Event& event) {
        if (enabled_ && pending_.size() < 64) pending_.push_back(nebula::workflow::observerJson(event));
    }

    void flush() {
        constexpr auto limit = 8 * 1024 * 1024;
        if (pending_.empty()) return;
        std::ofstream file(path_, std::ios::binary | std::ios::app);
        for (const auto& row : pending_) {
            if (!file || bytes_ + row.size() + 1 > limit) {
                enabled_ = false;
                break;
            }
            file << row << '\n';
            bytes_ += row.size() + 1;
        }
        file.flush();
        if (!file) enabled_ = false;
        if (!enabled_) std::cerr << "fleet edge: observer spool unavailable or full\n";
        pending_.clear();
    }

private:
    std::filesystem::path path_;
    std::uintmax_t bytes_{};
    bool enabled_{};
    std::vector<std::string> pending_;
};
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 6 && argc != 7) {
            throw Error(ErrorCode::invalid, "usage: nebula_fleet_edge 127.0.0.1:PORT ROBOT LEDGER STATIONS SIM_MS|nav2 [EVENTS_JSONL]");
        }
        localPort(argv[1]);
        const auto capabilities = number(argv[4]);
        const std::string_view adapter(argv[5]);
        const auto token = environmentToken("NEBULA_FLEET_ROBOT_TOKEN");
        SqliteEdgeLedger ledger(std::filesystem::path(argv[3]), argv[2]);
        auto navigation = navigationFor(adapter, argv[2], capabilities);
        const auto budget = adapter == "nav2" ? 45s : std::chrono::milliseconds{number(adapter)} + 250ms;
        TaskWorkflow task(*navigation, budget);
        EventFile events(argc == 7 ? argv[6] : nullptr);
        EdgeWorker worker(ledger, task, [&](const auto& event) { events.collect(event); }, [&] { events.flush(); });

        process::installStopHandlers();
        std::stop_source stop;
        std::jthread close([&](std::stop_token done) {
            while (!process::stopped && !done.stop_requested()) std::this_thread::sleep_for(15ms);
            stop.request_stop();
        });
        runEdgeSessions(argv[1], argv[2], token, capabilities, ledger, worker, stop.get_token());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fleet edge: " << error.what() << '\n';
        return 1;
    }
}
