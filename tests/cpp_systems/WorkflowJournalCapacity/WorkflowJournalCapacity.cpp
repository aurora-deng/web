#include "backend/systems/SqliteWorkflowJournal/SqliteWorkflowJournal.h"
#include "backend/workflow/DurableWorkflow/DurableWorkflow.h"
#include "backend/fleet/application/EdgeWorker/EdgeWorker.h"
#include "backend/fleet/infrastructure/SqliteEdgeLedger/SqliteEdgeLedger.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

using namespace nebula::workflow;
using nebula::systems::SqliteWorkflowJournal;
using nebula::systems::JournalLimits;
namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
void countEffect() {
    std::ofstream log("effects.txt", std::ios::app);
    log << "effect\n"; log.flush(); require(static_cast<bool>(log), "effect receipt unavailable");
}
void archive(const std::string& phase) {
    SqliteWorkflowJournal storage("archive.sqlite", "archive.snapshot.json");
    DurableWorkflow runner(storage, 2);
    const WorkflowGraph graph("capacity.v1", {
        {"prepare", {}, [](const NodeContext&) { return WorkResult{WorkStatus::success, "seed", {}}; }},
        {"effect", {"prepare"}, [](const NodeContext& context) {
            require(context.inputs.front().value == "seed", "checkpoint input lost");
            countEffect(); return WorkResult{WorkStatus::success, "done", {}};
        }, Effect::external},
    });
    if (phase == "populate") {
        for (unsigned i = 0; i < 260; ++i) {
            require(runner.run(graph, "same-input", {.runId = "run-" + std::to_string(i)}).state == State::success,
                    "archive stopped at historical capacity");
        }
        storage.open("unknown", "uncertain-definition", "");
        storage.start("unknown", "effect", Effect::external, 1);
        // Neither a terminal checkpoint nor an unresolved external operation is evicted.
        require(storage.node("run-0", "effect").result.has_value(), "old checkpoint was discarded");
    } else {
        require(runner.run(graph, "same-input", {.runId = "run-0"}).state == State::success, "old run cannot replay");
        require(storage.node("run-0", "effect").attempts == 1, "old effect was executed again");
        try { runner.run(graph, "different-input", {.runId = "run-0"}); throw std::runtime_error("identity conflict accepted"); }
        catch (const std::invalid_argument&) {}
        require(storage.node("unknown", "effect").attempts == 1 && !storage.node("unknown", "effect").result,
                "unknown outcome checkpoint disappeared");
        try { storage.start("unknown", "effect", Effect::external, 2); throw std::runtime_error("unknown external effect retried"); }
        catch (const std::invalid_argument&) {}
        storage.settle("unknown", "effect", {WorkStatus::success, "verified", {}});
        try { storage.settle("unknown", "review", {}); throw std::runtime_error("settled a node without a start"); }
        catch (const std::invalid_argument&) {}
        storage.start("run-0", "inspect", Effect::read, 1);
        storage.settle("run-0", "inspect", {});
    }
}
class Driver final : public nebula::fleet::NavigationSkill {
public:
    std::atomic_uint effects{};
    nebula::fleet::Capabilities stations() const noexcept override { return 1; }
    bool ready() override { return true; }
    nebula::fleet::NavigationResult navigate(nebula::fleet::Station, std::stop_token,
                                            std::chrono::steady_clock::time_point) override {
        ++effects; return nebula::fleet::NavigationResult::arrived;
    }
};
void capacity() {
    // A tiny configured run budget rejects admission, but existing receipts survive.
    {
        SqliteWorkflowJournal storage("capped.sqlite", {}, {1, 4 * 1024 * 1024});
        storage.open("kept", "definition", "");
        storage.start("kept", "effect", Effect::external, 1);
        storage.settle("kept", "effect", {WorkStatus::success, "receipt", {}});
        try { storage.open("new", "definition", ""); throw std::runtime_error("run cap not enforced"); }
        catch (const JournalCapacityExceeded&) {}
        require(!storage.open("kept", "definition", "").empty(), "full archive blocked replay");
        require(storage.node("kept", "effect").result->value == "receipt", "capacity failure destroyed receipt");
    }
    {
        SqliteWorkflowJournal storage("capped.sqlite", {}, {1, 4 * 1024 * 1024});
        require(storage.node("kept", "effect").result->value == "receipt", "restart lost receipt at capacity");
    }
    // A byte budget is independent of the run count budget. No real database is used.
    {
        SqliteWorkflowJournal storage("bytes.sqlite", {}, {50000, 4 * 1024 * 1024});
        unsigned count = 0; bool exhausted = false;
        for (; count < 64; ++count) {
            try { storage.open("large-" + std::to_string(count), std::string(128 * 1024, 'x'), ""); }
            catch (const JournalCapacityExceeded&) { exhausted = true; break; }
        }
        require(exhausted && count > 0, "byte admission budget not enforced");
        require(!storage.open("large-0", std::string(128 * 1024, 'x'), "").empty(), "byte cap blocked old identity");
    }
    require(std::filesystem::file_size("bytes.sqlite") <= 4 * 1024 * 1024, "database exceeded byte cap");
    // The graph and journal both bound each run to 128 checkpoints.
    {
        SqliteWorkflowJournal storage("nodes.sqlite"); storage.open("run", "definition", "");
        for (unsigned i = 0; i < 128; ++i) storage.start("run", "node-" + std::to_string(i), Effect::read, 1);
        try { storage.start("run", "too-many", Effect::read, 1); throw std::runtime_error("node cap not enforced"); }
        catch (const std::invalid_argument&) {}
    }
    // Exhaustion is a pre-navigation terminal failure, not a dead Edge worker.
    {
        using namespace nebula::fleet;
        using namespace std::chrono_literals;
        SqliteWorkflowJournal storage("worker.workflow.sqlite", {}, {1, 4 * 1024 * 1024});
        SqliteEdgeLedger ledger("worker.sqlite", "alpha");
        Driver driver; TaskWorkflow task(driver, 100ms, storage);
        EdgeWorker worker(ledger, task, {});
        for (unsigned i = 0; i < 3; ++i) {
            const Command command{"worker-" + std::to_string(i), "1234567890abcdef1234567890abcdef", Station::alpha, i + 1};
            ledger.stage(command);
            const auto accepted = ledger.pendingReceipt();
            require(accepted && accepted->phase == Phase::accepted, "acceptance receipt missing");
            ledger.confirm(*accepted); worker.permit();
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            std::optional<Receipt> receipt;
            while (std::chrono::steady_clock::now() < deadline) {
                receipt = ledger.pendingReceipt();
                if (receipt && (receipt->phase == Phase::succeeded || receipt->phase == Phase::failed)) break;
                require(worker.healthy(), "capacity killed Edge worker");
                std::this_thread::sleep_for(2ms);
            }
            require(receipt && receipt->phase == (i ? Phase::failed : Phase::succeeded), "capacity outcome not recorded");
            if (i) require(receipt->outcome == "workflow_capacity_exhausted", "capacity failure not classified");
            ledger.confirm(*receipt);
        }
        require(worker.healthy() && driver.effects.load() == 1, "capacity triggered motion or stopped worker");
    }
    std::cout << "capacity budgets, replay, byte cap and healthy Edge verified\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("expected populate, replay or capacity");
        const std::string phase = argv[1];
        if (phase == "capacity") capacity();
        else if (phase == "populate" || phase == "replay") archive(phase);
        else throw std::invalid_argument("unknown test phase");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
