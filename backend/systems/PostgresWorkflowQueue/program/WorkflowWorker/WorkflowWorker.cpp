#include "backend/systems/PostgresWorkflowQueue/PostgresWorkflowQueue.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace nebula::workflow;
using namespace std::chrono_literals;

namespace {

WorkflowGraph fixture(bool external) {
    const auto identity = [](const NodeContext& context) -> WorkResult {
        std::string value(context.idempotencyKey);
        for (const auto& input : context.inputs) value += "|" + input.value;
        return {WorkStatus::success, std::move(value), {}};
    };
    const auto effect = external ? Effect::external : Effect::idempotent;
    return WorkflowGraph(external ? "fork-join-external.v1" : "fork-join.v1", {
        {"A", {}, identity, effect, 5s, external ? 1U : 2U},
        {"B", {"A"}, identity}, {"C", {"A"}, identity}, {"D", {"B", "C"}, identity}
    });
}

unsigned milliseconds(std::string_view value) {
    unsigned result{};
    const auto end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end || result < 50 || result > 30000) {
        throw std::invalid_argument("invalid lease milliseconds");
    }
    return result;
}

}  // namespace

int main(int argc, char* argv[]) try {
    if (argc < 4) throw std::invalid_argument("usage: nebula_workflow_worker submit|work RUN read|external [OWNER LEASE_MS [hold]]");
    const auto* dsn = std::getenv("NEBULA_WORKFLOW_DSN");
    if (!dsn || !*dsn) throw std::invalid_argument("configure the isolated NEBULA_WORKFLOW_DSN");
    const std::string_view command = argv[1], run = argv[2], effect = argv[3];
    if (effect != "read" && effect != "external") throw std::invalid_argument("invalid registered fixture");
    auto graph = fixture(effect == "external");
    nebula::systems::PostgresWorkflowQueue db(dsn);
    if (command == "submit" && argc == 4) {
        std::cout << db.submit(run, graph, 40s) << '\n';
        return 0;
    }
    const bool hold = argc == 7 && std::string_view(argv[6]) == "hold";
    if (command != "work" || (argc != 6 && !hold)) throw std::invalid_argument("invalid worker arguments");
    const auto lease = Milliseconds(milliseconds(argv[5]));
    const auto deadline = Clock::now() + 41s;
    while (Clock::now() < deadline) {
        const auto beganClaim = Clock::now();
        auto claimed = db.claim(run, graph, argv[4], lease);
        if (!claimed) {
            const auto state = db.state(run);
            if (state != "started") {
                std::cout << "RESULT " << state << '\n';
                return state == "success" ? 0 : 2;
            }
            std::this_thread::sleep_for(25ms);
            continue;
        }
        std::cout << "CLAIM " << claimed->node << ' ' << claimed->attempt << ' ' << claimed->trace << std::endl;
        const auto step = std::ranges::find_if(graph.steps(), [&](const auto& row) { return row.node.id == claimed->node; });
        const auto key = claimed->run + ":" + claimed->node;
        const NodeContext context{claimed->run, claimed->trace, key, claimed->attempt,
                                  beganClaim + std::min(lease, claimed->remaining), {}, claimed->inputs};
        if (Clock::now() >= context.deadline) throw std::runtime_error("lease expired before handler entry");
        const auto result = step->node.execute(context);
        // An explicit lab flag holds the first completion so the controller
        // can demonstrate takeover without simulating a production handler.
        if (hold) {
            std::string input;
            if (!std::getline(std::cin, input) || input != "finish") return 2;
        }
        if (!db.finish(*claimed, result)) {
            std::cout << "STALE_LEASE\n";
            return 3;
        }
    }
    throw std::runtime_error("worker exceeded the lab deadline");
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
