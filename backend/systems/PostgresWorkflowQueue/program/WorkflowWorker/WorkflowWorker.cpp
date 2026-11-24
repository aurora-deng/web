#include "backend/systems/PostgresWorkflowQueue/PostgresWorkflowQueue.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace nebula::workflow;
using namespace std::chrono_literals;

namespace {

volatile std::sig_atomic_t stopping = 0;

void requestStop(int) { stopping = 1; }

WorkflowGraph fixture(std::string_view mode) {
    const auto identity = [](const NodeContext& context) -> WorkResult {
        std::string value(context.idempotencyKey);
        for (const auto& input : context.inputs) value += "|" + input.value;
        return {WorkStatus::success, std::move(value), {}};
    };
    const bool external = mode == "external";
    const bool slow = mode == "slow";
    const auto execute = [identity, slow](const NodeContext& context) -> WorkResult {
        if (slow) {
            const auto done = Clock::now() + 700ms;
            while (Clock::now() < done) {
                if (context.stop.stop_requested() || Clock::now() >= context.deadline) {
                    return {WorkStatus::retryable, {}, "handler_stopped"};
                }
                std::this_thread::sleep_for(5ms);
            }
        }
        return identity(context);
    };
    const auto effect = external ? Effect::external : Effect::idempotent;
    return WorkflowGraph(external ? "fork-join-external.v1" : slow ? "fork-join-slow.v1" : "fork-join.v1", {
        {"A", {}, execute, effect, 5s, external ? 1U : 2U},
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

bool executeLease(nebula::systems::PostgresWorkflowQueue& db, const nebula::systems::NodeLease& claim,
                  const WorkflowGraph& graph, Milliseconds lease, Clock::time_point beganClaim, bool hold) {
    const auto step = std::ranges::find_if(graph.steps(), [&](const auto& row) { return row.node.id == claim.node; });
    const auto key = claim.run + ":" + claim.node;
    const auto deadline = beganClaim + claim.remaining;
    if (step == graph.steps().end() || Clock::now() >= beganClaim + std::min(lease, claim.remaining)) return false;
    std::cout << "CLAIM " << claim.node << ' ' << claim.attempt << ' ' << claim.trace << std::endl;
    if (hold) {
        // Preserve a deterministic pause point for the two-process failure drill.
        const auto result = step->node.execute({claim.run, claim.trace, key, claim.attempt, deadline, {}, claim.inputs});
        std::string input;
        return std::getline(std::cin, input) && input == "finish" && db.finish(claim, result);
    }

    std::promise<WorkResult> completion;
    auto result = completion.get_future();
    const bool external = step->node.effect == Effect::external;
    std::jthread handler([&](std::stop_token stop) {
        try {
            completion.set_value(step->node.execute({claim.run, claim.trace, key, claim.attempt, deadline, stop, claim.inputs}));
        } catch (...) {
            completion.set_value({external ? WorkStatus::uncertain : WorkStatus::failure, {}, "handler_exception"});
        }
    });
    auto next = beganClaim + lease / 3;
    while (result.wait_until(std::min({next, deadline, Clock::now() + 50ms})) != std::future_status::ready) {
        const auto now = Clock::now();
        if (stopping || now >= deadline) {
            handler.request_stop();
            break;
        }
        if (now >= next) {
            if (!db.renew(claim, lease)) {
                handler.request_stop();
                return false;
            }
            next = now + lease / 3;
        }
    }
    auto value = result.get();
    if (stopping && value.status != WorkStatus::success) {
        value = {external ? WorkStatus::uncertain : WorkStatus::retryable, {}, "worker_stopping"};
    }
    return db.finish(claim, value);
}

void serve(nebula::systems::PostgresWorkflowQueue& db, std::string_view owner, Milliseconds lease) {
    const std::array registered{fixture("read"), fixture("external"), fixture("slow")};
    std::cout << "READY " << owner << std::endl;
    while (!stopping) {
        bool worked = false;
        for (const auto& candidate : db.readyRuns(registered)) {
            const auto graph = std::ranges::find_if(registered, [&](const auto& row) { return row.name() == candidate.workflow; });
            if (graph == registered.end()) continue;
            const auto beganClaim = Clock::now();
            if (auto claim = db.claim(candidate.run, *graph, owner, lease)) {
                if (!executeLease(db, *claim, *graph, lease, beganClaim, false)) std::cout << "STALE_LEASE\n";
                worked = true;
                break;
            }
        }
        if (!worked) std::this_thread::sleep_for(50ms);
    }
}

}  // namespace

int main(int argc, char* argv[]) try {
    if (argc < 4) throw std::invalid_argument("usage: nebula_workflow_worker submit|work RUN read|external|slow "
        "[OWNER LEASE_MS [hold]] | serve OWNER LEASE_MS | reconcile RUN NODE ATTEMPT success|failed EVIDENCE [VALUE]");
    const std::string_view command = argv[1];
    const auto* dsn = std::getenv(command == "reconcile" ? "NEBULA_WORKFLOW_RECONCILE_DSN" : "NEBULA_WORKFLOW_DSN");
    if (!dsn || !*dsn) throw std::invalid_argument("configure the dedicated workflow connection");
    nebula::systems::PostgresWorkflowQueue db(dsn);
    if (command == "reconcile") {
        if (argc != 7 && argc != 8) throw std::invalid_argument("reconcile needs a verified verdict and evidence ID");
        const std::string_view attempt = argv[4], verdict = argv[5];
        if (attempt.size() != 1 || attempt[0] < '1' || attempt[0] > '3' ||
            (verdict != "success" && verdict != "failed")) throw std::invalid_argument("invalid verification");
        const bool saved = db.reconcile(argv[2], argv[3], attempt[0] - '0', verdict == "success", argv[6],
                                        argc == 8 ? argv[7] : "");
        std::cout << (saved ? "VERIFIED\n" : "VERIFICATION_CONFLICT\n");
        return saved ? 0 : 3;
    }
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    if (command == "serve" && argc == 4) {
        if (!validIdentifier(argv[2], 64)) throw std::invalid_argument("invalid workflow Worker ID");
        serve(db, argv[2], Milliseconds(milliseconds(argv[3])));
        return 0;
    }
    const std::string_view run = argv[2], effect = argv[3];
    if (effect != "read" && effect != "external" && effect != "slow") throw std::invalid_argument("invalid registered fixture");
    auto graph = fixture(effect);
    if (command == "submit" && argc == 4) {
        std::cout << db.submit(run, graph, 40s) << '\n';
        return 0;
    }
    const bool hold = argc == 7 && std::string_view(argv[6]) == "hold";
    if (command != "work" || (argc != 6 && !hold)) throw std::invalid_argument("invalid worker arguments");
    const auto lease = Milliseconds(milliseconds(argv[5]));
    const auto deadline = Clock::now() + 41s;
    while (!stopping && Clock::now() < deadline) {
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
        if (!executeLease(db, *claimed, graph, lease, beganClaim, hold)) {
            std::cout << "STALE_LEASE\n";
            return 3;
        }
    }
    if (stopping) return 0;
    throw std::runtime_error("worker exceeded the lab deadline");
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
