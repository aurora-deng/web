#include "backend/fleet/application/EdgeWorker/EdgeWorker.h"
#include "backend/fleet/application/FleetCoordinator/FleetCoordinator.h"
#include "backend/fleet/application/TaskWorkflow/TaskWorkflow.h"
#include "backend/fleet/infrastructure/SimNavigation/SimNavigation.h"
#include "backend/fleet/infrastructure/SqliteEdgeLedger/SqliteEdgeLedger.h"
#include "backend/fleet/infrastructure/SqliteFleetLedger/SqliteFleetLedger.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stop_token>
#include <thread>

using namespace nebula::fleet;
using namespace std::chrono_literals;

namespace {
constexpr auto trace = "05238dcef4a0248eb46b760fa17da291";

void require(bool passed, const char* title) {
    if (!passed) throw std::runtime_error(title);
}

template<class F>
void expect(ErrorCode code, F&& execute) {
    try { execute(); }
    catch (const Error& error) {
        if (error.code() == code) return;
        throw;
    }
    throw std::runtime_error("fleet operation should have failed");
}

void testScheduler(const std::filesystem::path& database) {
    auto now = std::chrono::steady_clock::now();
    SqliteFleetLedger ledger(database, 2);
    FleetCoordinator coordinator(ledger, 10ms, [&] { return now; });
    coordinator.submit("a", Station::alpha, trace);
    coordinator.submit("b", Station::beta, trace);
    expect(ErrorCode::exhausted, [&] { coordinator.submit("c", Station::alpha, trace); });
    require(coordinator.submit("a", Station::alpha, trace).phase == Phase::queued, "replay failed at queue limit");
    expect(ErrorCode::conflict, [&] { coordinator.submit("a", Station::beta, trace); });

    auto alice = coordinator.connect("alice", 1);
    auto bob = coordinator.connect("bob", 2);
    expect(ErrorCode::conflict, [&] { coordinator.connect("bob", 2); });
    auto a = coordinator.exchange(alice, {});
    auto b = coordinator.exchange(bob, {});
    require(a && b && a->taskId == "a" && b->taskId == "b" && a->epoch != b->epoch, "capability routing failed");

    Receipt aAccepted{"a", a->epoch, Phase::accepted, ""};
    expect(ErrorCode::conflict, [&] { coordinator.exchange(bob, aAccepted); });
    auto stale = aAccepted;
    ++stale.epoch;
    expect(ErrorCode::conflict, [&] { coordinator.exchange(alice, stale); });
    coordinator.exchange(alice, aAccepted);
    coordinator.exchange(alice, aAccepted);
    coordinator.submit("c", Station::alpha, trace);
    require(!coordinator.exchange(alice, {}), "active robot received another task");
    now += 11ms;
    require(coordinator.status("a").needsReconciliation, "heartbeat expiry is invisible");
    const auto replacement = coordinator.connect("alice", 1);
    expect(ErrorCode::offline, [&] { coordinator.exchange(alice, {}); });
    coordinator.disconnect(alice);
    require(coordinator.status("a").connected, "old stream removed current session");
    require(!coordinator.exchange(replacement, {}), "offline work was reassigned");
    const auto c = coordinator.exchange(replacement, Receipt{"a", a->epoch, Phase::succeeded, "arrived"});
    require(c && c->taskId == "c", "next eligible task was not dispatched after completion");
    expect(ErrorCode::conflict, [&] { coordinator.exchange(replacement, aAccepted); });
}

void testEdgeRecovery(const std::filesystem::path& database) {
    Command work{"a", trace, Station::alpha, 42};
    {
        SqliteEdgeLedger edge(database, "alice");
        expect(ErrorCode::storage, [&] { SqliteEdgeLedger duplicate(database, "alice"); });
        edge.stage(work);
        edge.stage(work);
        auto conflict = work;
        ++conflict.epoch;
        expect(ErrorCode::conflict, [&] { edge.stage(conflict); });
        require(!edge.beginReady(), "navigation began without a durable acceptance echo");
        const auto receipt = edge.pendingReceipt();
        require(receipt && receipt->phase == Phase::accepted, "acceptance missing");
        edge.confirm(*receipt);
        require(edge.beginReady() == work && !edge.beginReady(), "same command started twice");
    }
    {
        SqliteEdgeLedger recovered(database, "alice");
        const auto doubtful = recovered.pendingReceipt();
        require(doubtful && doubtful->phase == Phase::uncertain && doubtful->outcome == "edge_restart",
                "crash recovery retried an unknown action");
        recovered.stage(work);
        require(!recovered.beginReady(), "unknown action can run again");
        expect(ErrorCode::conflict, [&] { recovered.stage(Command{"b", trace, Station::alpha, 43}); });
    }
    expect(ErrorCode::conflict, [&] { SqliteEdgeLedger wrongOwner(database, "another"); });
}

void testNavigation() {
    SimNavigation navigation(1, 1ms);
    TaskWorkflow task(navigation, 100ms);
    std::size_t total = 0;
    const auto arrived = task.run({"a", trace, Station::alpha, 1}, {}, [&](const auto&) { ++total; });
    require(arrived.phase == Phase::succeeded && total == 22, "fleet did not run through the backend DAG");
    const auto rejected = task.run({"b", trace, Station::beta, 2}, {}, {});
    require(rejected.phase == Phase::failed, "unsupported navigation reached the actuator");
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = task.run({"c", trace, Station::alpha, 3}, stop.get_token(), {});
    require(cancelled.phase == Phase::failed && cancelled.code == "cancelled_before_navigation", "unsafe cancel classification");
}

void testWorkerShutdown(const std::filesystem::path& database) {
    SqliteEdgeLedger ledger(database, "worker");
    ledger.stage({"slow", trace, Station::alpha, 1});
    ledger.confirm(*ledger.pendingReceipt());
    SimNavigation navigation(1, 4s);
    TaskWorkflow task(navigation, 5s);
    std::atomic_bool launched{false};
    {
        EdgeWorker worker(ledger, task, [&](const nebula::workflow::Event& event) {
            if (event.nodeId == "navigate" && event.state == nebula::workflow::State::started) {
                launched.store(true);
            }
        });
        worker.permit();
        const auto until = std::chrono::steady_clock::now() + 2s;
        while (!launched.load() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(1ms);
        }
        require(launched.load() && worker.healthy(), "edge worker failed to launch");
        const auto active = ledger.pendingReceipt();
        require(active && active->phase == Phase::executing, "executing boundary missing from inbox");
        // jthread destruction cancels and joins while the ledger and DAG live.
    }
    const auto outcome = ledger.pendingReceipt();
    require(outcome && outcome->phase == Phase::uncertain && !ledger.beginReady(),
            "shutdown did not preserve the unknown outcome");
}
}

int main() {
    const auto stem = std::filesystem::temp_directory_path() / ("nebula-" + nebula::workflow::newTraceId());
    const auto fleet = stem.string() + "-fleet.sqlite";
    const auto edge = stem.string() + "-edge.sqlite";
    const auto worker = stem.string() + "-worker.sqlite";
    int exit = 0;
    try {
        testScheduler(fleet);
        testEdgeRecovery(edge);
        testNavigation();
        testWorkerShutdown(worker);
        std::cout << "Fleet contracts passed: bounded dispatch, sessions, durable recovery and native workflow\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        exit = 1;
    }
    std::error_code cleanup;
    std::filesystem::remove(fleet, cleanup);
    std::filesystem::remove(edge, cleanup);
    std::filesystem::remove(worker, cleanup);
    return exit;
}
