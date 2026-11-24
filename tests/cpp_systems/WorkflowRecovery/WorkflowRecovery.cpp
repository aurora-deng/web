#include "backend/systems/SqliteWorkflowJournal/SqliteWorkflowJournal.h"
#include "backend/workflow/DurableWorkflow/DurableWorkflow.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace nebula::workflow;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string effectRecord(const std::string& path) {
    std::ifstream record(path);
    std::string key;
    std::getline(record, key);
    return key;
}

}

int main(int argc, char* argv[]) {
    try {
        if (argc != 4) throw std::invalid_argument("expected DB, effect and phase");
        const std::string path = argv[1];
        const std::string effectFile = path + ".effect";
        const std::string_view kind = argv[2];
        const std::string_view phase = argv[3];
        require(kind == "external" || kind == "idempotent", "unexpected effect");
        const auto effect = kind == "external" ? Effect::external : Effect::idempotent;
        nebula::systems::SqliteWorkflowJournal storage(path);
        DurableWorkflow runner(storage, 2);
        const WorkflowGraph graph("recovery.v1", {
            {"source", {}, [](const NodeContext&) { return WorkResult{WorkStatus::success, "seed", {}}; }},
            {"action", {"source"}, [&](const NodeContext& context) {
                require(context.inputs.front().value == "seed", "checkpoint lost dependency value");
                const auto existing = effectRecord(effectFile);
                if (existing.empty()) {
                    std::ofstream record(effectFile, std::ios::binary);
                    record << context.idempotencyKey << '\n';
                    record.close();
                    require(static_cast<bool>(record), "fixture could not commit external effect");
                } else {
                    require(effect == Effect::idempotent && existing == context.idempotencyKey,
                            "unsafe retry or changed idempotency key");
                }
                if (phase == "crash") std::_Exit(86);
                return WorkResult{WorkStatus::success, "committed", {}};
            }, effect, Milliseconds{1000}, effect == Effect::external ? 1U : 2U},
            {"report", {"action"}, [](const NodeContext& context) {
                require(context.inputs.front().value == "committed", "settled result was not restored");
                return WorkResult{};
            }},
        });
        if (phase == "reconcile") {
            require(effectRecord(effectFile) == "proof:action", "effect receipt not available");
            runner.reconcile("proof", "action", {WorkStatus::success, "committed", {}});
        }
        if (phase == "conflict") {
            try { runner.run(graph, "different-input", {.runId = "proof"}); }
            catch (const std::invalid_argument&) { return 0; }
            throw std::runtime_error("checkpoint was reused for a different input");
        }
        const auto result = runner.run(graph, "fixture-1", {.runId = "proof"});
        const auto checkpoint = storage.node("proof", "action");
        if (kind == "external" && phase == "blocked") {
            require(result.state == State::uncertain && result.nodes[1].errorCode == "requires_reconciliation" &&
                    result.nodes.back().state == State::skipped && checkpoint.attempts == 1 && !checkpoint.result,
                    "unknown external action escaped checkpoint");
        } else {
            require(result.state == State::success && checkpoint.result.has_value(), "checkpoint did not complete");
            require(checkpoint.attempts == (effect == Effect::external ? 1U : 2U), "persisted attempt count is wrong");
        }
        std::cout << result.traceId << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
