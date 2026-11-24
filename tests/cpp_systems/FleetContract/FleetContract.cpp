#include "backend/fleet/application/EdgeWorker/EdgeWorker.h"
#include "backend/fleet/application/FleetCoordinator/FleetCoordinator.h"
#include "backend/fleet/application/TaskWorkflow/TaskWorkflow.h"
#include "backend/fleet/domain/RobotSkills/RobotSkills.h"
#include "backend/fleet/infrastructure/SimNavigation/SimNavigation.h"
#include "backend/fleet/infrastructure/SqliteDatabase/SqliteDatabase.h"
#include "backend/fleet/infrastructure/SqliteEdgeLedger/SqliteEdgeLedger.h"
#include "backend/fleet/infrastructure/SqliteFleetLedger/SqliteFleetLedger.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <latch>
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

void testScheduler(const std::filesystem::path& database, const char*& scenario) {
    auto now = std::chrono::steady_clock::now();
    SqliteFleetLedger ledger(database, 2);
    FleetCoordinator coordinator(ledger, 10ms, [&] { return now; });
    scenario = "scheduler initial submission";
    coordinator.submit("a", Station::alpha, trace);
    scenario = "scheduler queue limit and idempotency";
    coordinator.submit("b", Station::beta, trace);
    expect(ErrorCode::exhausted, [&] { coordinator.submit("c", Station::alpha, trace); });
    require(coordinator.submit("a", Station::alpha, trace).phase == Phase::queued, "replay failed at queue limit");
    expect(ErrorCode::conflict, [&] { coordinator.submit("a", Station::beta, trace); });

    scenario = "scheduler connect and route";
    auto alice = coordinator.connect("alice", 1);
    auto bob = coordinator.connect("bob", 2);
    expect(ErrorCode::conflict, [&] { coordinator.connect("bob", 2); });
    auto a = coordinator.exchange(alice, {});
    auto b = coordinator.exchange(bob, {});
    require(a && b && a->taskId == "a" && b->taskId == "b" && a->epoch != b->epoch, "capability routing failed");

    scenario = "scheduler validate receipt and retain the active task";
    Receipt aAccepted{"a", a->epoch, Phase::accepted, ""};
    expect(ErrorCode::conflict, [&] { coordinator.exchange(bob, aAccepted); });
    auto stale = aAccepted;
    ++stale.epoch;
    expect(ErrorCode::conflict, [&] { coordinator.exchange(alice, stale); });
    coordinator.exchange(alice, aAccepted);
    coordinator.exchange(alice, aAccepted);
    coordinator.submit("c", Station::alpha, trace);
    require(!coordinator.exchange(alice, {}), "active robot received another task");
    scenario = "scheduler expire, reattach and reconcile";
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

void testSimulationReview(const std::filesystem::path& database) {
    constexpr std::int64_t now = 1000;
    ReviewDraft first{nebula::workflow::newTraceId(), "source", 0};
    std::string replacement;
    {
        SqliteFleetLedger ledger(database, 1);
        const auto source = ledger.submit(first.sourceTaskId, Station::alpha, trace);
        first.sourceEpoch = source.command.epoch;
        require(ledger.stageReview(first, now).state == ReviewState::pending, "proposal not staged");
        require(ledger.stageReview(first, now + 1).createdAt == now, "duplicate staging changed immutable evidence");
        auto different = first;
        ++different.sourceEpoch;
        expect(ErrorCode::conflict, [&] { ledger.stageReview(different, now); });
        expect(ErrorCode::invalid, [&] {
            ledger.submit(std::string(replacementPrefix) + first.traceId, Station::alpha, trace);
        });
        require(ledger.lookup("source")->phase == Phase::queued, "pending approval touched the queue");
    }
    {
        SqliteFleetLedger ledger(database, 1);
        require(ledger.inspectReview(first.traceId, now + 2).draft == first, "pending proposal lost on restart");
        const auto approved = ledger.decideReview(first.traceId, Decision::approve, now + 3);
        replacement = approved.replacementTaskId;
        require(approved.state == ReviewState::approved && !replacement.empty(), "approval failed at queue capacity");
        require(ledger.lookup("source")->outcome == supersededOutcome &&
                ledger.lookup(replacement)->command.traceId == first.traceId, "source and replacement not stored together");
        require(ledger.dispatch("robot", 1)->taskId == replacement, "original task could still be dispatched");
        require(ledger.decideReview(first.traceId, Decision::reject, now + 4).state == ReviewState::approved,
                "retry with different decision changed terminal approval");
    }
    SqliteFleetLedger ledger(database, 3);
    require(ledger.decideReview(first.traceId, Decision::approve, now + 1000).replacementTaskId == replacement,
            "approved result did not survive restart or deadline");
    const auto beta = ledger.submit("beta-source", Station::beta, trace);
    ReviewDraft stale{nebula::workflow::newTraceId(), beta.command.taskId, beta.command.epoch};
    ReviewDraft expired{nebula::workflow::newTraceId(), beta.command.taskId, beta.command.epoch};
    ReviewDraft rejected{nebula::workflow::newTraceId(), beta.command.taskId, beta.command.epoch};
    for (const auto& draft : {stale, expired, rejected}) ledger.stageReview(draft, now);
    require(ledger.decideReview(expired.traceId, Decision::approve, now + reviewLifetimeSeconds).state == ReviewState::expired,
            "expired proposal reached the queue");
    require(ledger.decideReview(rejected.traceId, Decision::reject, now + 1).state == ReviewState::rejected &&
            ledger.decideReview(rejected.traceId, Decision::approve, now + 2).state == ReviewState::rejected,
            "rejected proposal could be approved");
    ledger.dispatch("beta-robot", 2);
    require(ledger.decideReview(stale.traceId, Decision::approve, now + 3).state == ReviewState::stale &&
            ledger.lookup(beta.command.taskId)->phase == Phase::leased, "approval changed a leased task");
    for (const auto& draft : {stale, expired, rejected}) {
        require(!ledger.lookup(std::string(replacementPrefix) + draft.traceId), "refused proposal created a task");
    }
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

void testHistoricalCapacity(const std::filesystem::path& fleet, const std::filesystem::path& edgeFile) {
    {
        SqliteFleetLedger ledger(fleet, 1);
        SqliteEdgeLedger edge(edgeFile, "archive");
    }
    {
        sqlite::Database existing(fleet, "fleet", "");
        existing.exec(R"SQL(
            CREATE INDEX IF NOT EXISTS tasks_queue ON tasks(phase,epoch);
            INSERT INTO tasks(epoch,task_id,station,trace_id,robot_id,phase,outcome)
            VALUES(50000,'archived',1,'05238dcef4a0248eb46b760fa17da291','archive',5,'arrived');
            WITH RECURSIVE seq(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM seq WHERE n<4096)
            INSERT INTO agent_sim_reviews(trace_id,source_task_id,source_epoch,station,created_at,expires_at)
            SELECT printf('%032x',n),'archived',50000,1,1,2 FROM seq;
        )SQL");
        sqlite::Database oldEdge(edgeFile, "edge", "archive");
        oldEdge.exec(R"SQL(
            CREATE INDEX IF NOT EXISTS inbox_report ON inbox(reported,epoch);
            WITH RECURSIVE seq(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM seq WHERE n<50000)
            INSERT INTO inbox(task_id,trace_id,station,epoch,phase,outcome,reported,starts)
            SELECT 'old-'||n,'05238dcef4a0248eb46b760fa17da291',1,n,5,'arrived',1,1 FROM seq;
        )SQL");
    }
    SqliteFleetLedger ledger(fleet, 1);
    const auto current = ledger.submit("new-task", Station::alpha, trace);
    require(current.command.epoch == 50001, "historical epoch stopped new admission or was reused");
    const auto recent = ledger.listBefore(0, 1);
    const auto older = ledger.listBefore(current.command.epoch, 1);
    require(recent.size() == 1 && recent.front().command == current.command &&
            older.size() == 1 && older.front().command.epoch == 50000,
            "task paging lost records beyond the old epoch ceiling");
    require(ledger.submit("archived", Station::alpha, trace).phase == Phase::succeeded,
            "terminal task identity did not deduplicate at live capacity");
    ledger.apply("archive", {"archived", 50000, Phase::succeeded, "arrived"});
    expect(ErrorCode::exhausted, [&] { ledger.submit("overload", Station::alpha, trace); });
    require(ledger.stageReview({nebula::workflow::newTraceId(), current.command.taskId, current.command.epoch}, 1000)
                .state == ReviewState::pending,
            "expired approvals blocked live proposal admission");

    SqliteEdgeLedger edge(edgeFile, "archive");
    edge.stage({"old-50000", trace, Station::alpha, 50000});
    require(!edge.pendingReceipt() && !edge.beginReady(), "archived edge command was executed again");
    edge.stage(current.command);
    const auto next = edge.pendingReceipt();
    require(next && next->epoch == 50001 && !edge.beginReady(), "edge failed to accept a new command");
    edge.confirm(*next);
    require(edge.beginReady() == current.command, "fresh edge task ignored the acknowledgment boundary");
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

class ControlledNavigation final : public NavigationSkill {
public:
    Capabilities stations() const noexcept override { return 1; }
    bool ready() override { return permitted; }
    NavigationResult navigate(const Command&, std::stop_token stop, std::chrono::steady_clock::time_point deadline) override {
        ++calls;
        started.count_down();
        while (!finished && !stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        return finished ? NavigationResult::arrived : NavigationResult::uncertain;
    }

    bool permitted{true};
    std::atomic_uint calls{};
    std::atomic_bool finished{};
    std::latch started{1};
};

void testSkillContract() {
    constexpr std::array supported{SkillReference{getRobotStatus.id, 1}, SkillReference{navigateToSimStation.id, 1},
                                   SkillReference{reportTaskOutcome.id, 1}};
    auto wrong = supported;
    wrong[1].version = 2;
    require(validRobotSkills(supported) && !validRobotSkills(wrong), "negotiation did not validate skill versions");
    wrong[1] = wrong[0];
    require(!validRobotSkills(wrong), "negotiation accepted duplicate skills");
    require(resolveRobotSkill(supported[0])->effect == RobotEffect::read &&
            resolveRobotSkill(supported[1])->effect == RobotEffect::external &&
            resolveRobotSkill(supported[2])->effect == RobotEffect::idempotent, "skill effect changed");

    ControlledNavigation driver;
    TaskWorkflow first(driver, 1000ms);
    TaskWorkflow contender(driver, 1000ms);
    const Command a{"a", trace, Station::alpha, 1};
    const Command b{"b", trace, Station::alpha, 2};
    driver.permitted = false;
    require(first.run(a, {}, {}).phase == Phase::failed && driver.calls == 0, "unready skill entered chassis");
    driver.permitted = true;
    Outcome result;
    std::jthread occupied([&](std::stop_token stop) { result = first.run(a, stop, {}); });
    driver.started.wait();
    require(contender.run(b, {}, {}).code == "chassis_busy" && driver.calls == 1, "two graphs owned the same chassis");
    driver.finished = true;
    occupied.join();
    require(result.phase == Phase::succeeded, "first chassis owner did not finish");

    ControlledNavigation cancelled;
    TaskWorkflow cancelledRun(cancelled, 1000ms);
    std::jthread executing([&](std::stop_token stop) { result = cancelledRun.run(a, stop, {}); });
    cancelled.started.wait();
    executing.request_stop();
    executing.join();
    require(result.phase == Phase::uncertain && cancelled.claimChassis().owns_lock(), "cancelled action lost safety fence or lease");

    ControlledNavigation timeout;
    TaskWorkflow timedRun(timeout, 200ms);
    require(timedRun.run(a, {}, {}).phase == Phase::uncertain && timeout.calls == 1,
            "navigation timeout was mistaken for proof of refusal");
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
    const auto review = stem.string() + "-review.sqlite";
    const auto edge = stem.string() + "-edge.sqlite";
    const auto worker = stem.string() + "-worker.sqlite";
    const auto historicalFleet = stem.string() + "-historic-fleet.sqlite";
    const auto historicalEdge = stem.string() + "-historic-edge.sqlite";
    int exit = 0;
    const char* scenario = "scheduler initialization";
    try {
        testScheduler(fleet, scenario);
        scenario = "simulation approval";
        testSimulationReview(review);
        scenario = "edge recovery";
        testEdgeRecovery(edge);
        scenario = "historical ledger and live capacity";
        testHistoricalCapacity(historicalFleet, historicalEdge);
        scenario = "navigation workflow";
        testNavigation();
        scenario = "skill contract";
        testSkillContract();
        scenario = "worker shutdown";
        testWorkerShutdown(worker);
        std::cout << "Fleet contracts passed: bounded dispatch, durable review, sessions, recovery and native workflow\n";
    } catch (const std::exception& error) {
        std::cerr << scenario << ": " << error.what() << '\n';
        exit = 1;
    }
    std::error_code cleanup;
    std::filesystem::remove(fleet, cleanup);
    std::filesystem::remove(review, cleanup);
    std::filesystem::remove(edge, cleanup);
    std::filesystem::remove(worker, cleanup);
    std::filesystem::remove(historicalFleet, cleanup);
    std::filesystem::remove(historicalEdge, cleanup);
    return exit;
}
