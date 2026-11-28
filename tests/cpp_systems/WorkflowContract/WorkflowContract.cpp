#include "backend/workflow/DagExecutor/DagExecutor.h"
#include "backend/workflow/ObserverEvents/ObserverEvents.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace nebula::workflow;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
}

const NodeResult& node(const RunResult& result, std::string_view name) {
    for (const auto& item : result.nodes) if (item.nodeId == name) return item;
    throw std::runtime_error("expected node absent");
}

Node simple(std::string name, std::vector<std::string> needs = {}) {
    return {std::move(name), std::move(needs), [](const NodeContext& c) {
        return WorkResult{WorkStatus::success, std::to_string(c.inputs.size()), {}};
    }};
}

void graphValidation() {
    const auto reject = [](std::vector<Node> nodes) {
        try {
            WorkflowGraph invalid("invalid", std::move(nodes));
        } catch (const std::invalid_argument&) {
            return;
        }
        throw std::runtime_error("invalid graph was accepted");
    };
    reject({simple("a"), simple("a")});
    reject({simple("a", {"b"}), simple("b", {"a"})});
    reject({simple("a", {"missing"})});
    reject({simple("a"), simple("b", {"a", "a"})});
    auto irreversible = simple("action");
    irreversible.effect = Effect::external;
    irreversible.attempts = 2;
    reject({std::move(irreversible)});
}

WorkflowGraph fiveNodes() {
    return WorkflowGraph("verify.fork_join.v1", {simple("audit", {"join"}), simple("join", {"b", "c"}),
                         simple("c", {"start"}), simple("b", {"start"}), simple("start")});
}

void forksAndJoins(const char* eventPath) {
    auto graph = fiveNodes();
    DagExecutor executor(2);
    for (int seed = 0; seed < 100; ++seed) {
        unsigned terminalEvents = 0;
        unsigned joinEvents = 0;
        std::ofstream sink;
        if (seed == 0 && eventPath) {
            sink.open(eventPath, std::ios::binary | std::ios::trunc);
            require(static_cast<bool>(sink), "event output cannot be opened");
        }
        const auto result = executor.run(graph, {.runId = "fixture." + std::to_string(seed),
            .onEvent = [&](const Event& e) {
                if (e.kind == EventKind::run && e.state != State::started) ++terminalEvents;
                if (e.kind == EventKind::node && e.nodeId == "join" && e.state == State::success) ++joinEvents;
                if (sink.is_open()) sink << observerJson(e) << '\n';
            }});
        require(result.state == State::success && node(result, "join").value == "2", "join lost a parent output");
        require(node(result, "audit").value == "1" && terminalEvents == 1 && joinEvents == 1, "terminal count");
        require(result.peakActive == 2 && result.peakReady == 2, "fork did not occupy configured worker slots");
    }

    std::latch both(2);
    auto first = [&](const NodeContext&) { both.arrive_and_wait(); return WorkResult{}; };
    const WorkflowGraph parallel("parallel.v1", {Node{"b", {}, first}, Node{"c", {}, first}, simple("join", {"b","c"})});
    require(executor.run(parallel, {.runId = "parallel"}).state == State::success, "actual workers did not overlap");
}

void retriesAndBranchFailure() {
    DagExecutor executor(2);
    std::string key;
    auto retry = simple("idempotent");
    retry.effect = Effect::idempotent;
    retry.attempts = 2;
    retry.backoff = 0ms;
    retry.execute = [&](const NodeContext& c) {
        if (c.attempt == 1) {
            key = c.idempotencyKey;
            return WorkResult{WorkStatus::retryable, {}, {}};
        }
        require(key == c.idempotencyKey, "logical operation key changed between attempts");
        return WorkResult{};
    };
    auto failed = simple("other");
    failed.execute = [](const NodeContext&) -> WorkResult { throw std::runtime_error("private detail"); };
    WorkflowGraph graph("retry.v1", {retry, failed, simple("join", {"idempotent", "other"})});
    const auto result = executor.run(graph, {.runId = "retry"});
    require(result.state == State::failed && node(result, "idempotent").attempts == 2 &&
            node(result, "idempotent").state == State::success, "retry or branch isolation failed");
    require(node(result, "other").errorCode == "execution_exception" &&
            node(result, "join").state == State::skipped, "failed branch was forwarded");

    const auto operationKey = [&](std::string run, std::string nodeId) {
        auto action = simple(std::move(nodeId));
        action.execute = [](const NodeContext& context) {
            return WorkResult{WorkStatus::success, std::string(context.idempotencyKey), {}};
        };
        return executor.run(WorkflowGraph("identities.v1", {action}), {.runId = std::move(run)}).nodes.front().value;
    };
    require(operationKey("work.batch", "write") != operationKey("work", "batch.write"),
            "different run and node identities shared an operation key");
}

void deadlineAndLateResult() {
    auto slow = simple("late");
    slow.timeout = 25ms;
    std::atomic_bool left{false};
    slow.execute = [&](const NodeContext&) {
        std::this_thread::sleep_for(100ms);
        left = true;
        return WorkResult{WorkStatus::success, "late-value", {}};
    };
    DagExecutor executor(1);
    WorkflowGraph graph("timeout.v1", {slow, simple("next", {"late"})});
    const auto result = executor.run(graph, {.runId = "timeout"});
    require(left && result.state == State::timeout && node(result, "late").value.empty() &&
            node(result, "next").state == State::skipped, "late completion escaped timeout fence");

    std::atomic_int ticks{0};
    slow.execute = [&](const NodeContext&) {
        ticks.store(30);
        return WorkResult{WorkStatus::success, "virtual-late", {}};
    };
    const WorkflowGraph virtualGraph("virtual_deadline.v1", {slow, simple("next", {"late"})});
    const auto virtualResult = executor.run(virtualGraph, {.runId = "virtual-timeout", .now = [&] {
        return Clock::time_point{Milliseconds{ticks.load()}};
    }});
    require(virtualResult.state == State::timeout && node(virtualResult, "next").state == State::skipped,
            "injected clock did not fence a late result");
}

void cancellationAndAdmission() {
    auto active = simple("active");
    std::latch entered(1);
    std::atomic_bool left{false};
    active.execute = [&](const NodeContext& ctx) {
        entered.count_down();
        while (!ctx.stop.stop_requested()) std::this_thread::sleep_for(1ms);
        left = true;
        return WorkResult{};
    };
    DagExecutor executor(1);
    const WorkflowGraph graph("cancel.v1", {active, simple("queued"), simple("unstarted", {"active"})});
    RunResult result;
    std::jthread run([&](std::stop_token stop) {
        result = executor.run(graph, {.runId = "cancel", .stop = stop});
    });
    entered.wait();
    bool busy = false;
    try { executor.run(graph, {.runId = "another"}); }
    catch (const std::runtime_error&) { busy = true; }
    run.request_stop();
    run.join();
    require(busy && left && result.state == State::cancelled && node(result, "queued").state == State::cancelled &&
            node(result, "unstarted").attempts == 0, "cancellation or single-run admission failed");
}

void multipleWorkflowDefinitions() {
    DagExecutor executor(2);
    std::vector<std::string> completed;
    const auto record = [&](const Event& event) {
        if (event.kind == EventKind::run && event.state == State::success) {
            completed.push_back(std::string(event.workflow) + ":" + std::string(event.runId));
        }
    };
    const WorkflowGraph first("catalog.first.v1", {{"shared_node", {}, [](const NodeContext& context) {
        return WorkResult{WorkStatus::success, "first:" + std::string(context.runId), {}};
    }}});
    const WorkflowGraph second("catalog.second.v1", {
        {"shared_node", {}, [](const NodeContext& context) {
            return WorkResult{WorkStatus::success, "second:" + std::string(context.runId), {}};
        }},
        {"join", {"shared_node"}, [](const NodeContext& context) {
            return WorkResult{WorkStatus::success, context.inputs.front().value, {}};
        }},
    });
    const auto a = executor.run(first, {.runId = "first-run", .onEvent = record});
    const auto b = executor.run(second, {.runId = "second-run", .onEvent = record});
    const auto c = executor.run(first, {.runId = "first-again", .onEvent = record});
    require(a.state == State::success && b.state == State::success && c.state == State::success,
            "different workflow definitions must use the same generic executor");
    require(node(a, "shared_node").value == "first:first-run" &&
            node(b, "join").value == "second:second-run" &&
            node(c, "shared_node").value == "first:first-again",
            "workflow results must not leak across definitions or runs");
    require(completed == std::vector<std::string>{"catalog.first.v1:first-run",
            "catalog.second.v1:second-run", "catalog.first.v1:first-again"},
            "workflow event identities must preserve the selected definition");
}

void benchmark() {
    const auto graph = fiveNodes();
    std::cout << "workers,round,runs,p50_us,p95_us,p99_us,peak_ready\n";
    for (const unsigned workers : {1U, 2U, 4U}) {
        DagExecutor executor(workers);
        for (unsigned round = 1; round <= 3; ++round) {
            std::array<double, 1000> elapsed{};
            std::size_t peak = 0;
            for (std::size_t i = 0; i < elapsed.size(); ++i) {
                const auto start = Clock::now();
                const auto result = executor.run(graph, {.runId = "bench"});
                elapsed[i] = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
                require(result.state == State::success, "benchmark run failed");
                peak = std::max(peak, result.peakReady);
            }
            std::ranges::sort(elapsed);
            std::cout << workers << ',' << round << ',' << elapsed.size() << ',' << elapsed[499]
                      << ',' << elapsed[949] << ',' << elapsed[989] << ',' << peak << '\n';
        }
    }
}
}

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
            benchmark();
            return 0;
        }
        if (argc != 1 && (argc != 3 || std::string_view(argv[1]) != "--events")) {
            throw std::invalid_argument("usage: nebula_workflow_contract [--benchmark | --events PATH]");
        }
        graphValidation();
        forksAndJoins(argc == 3 ? argv[2] : nullptr);
        retriesAndBranchFailure();
        deadlineAndLateResult();
        cancellationAndAdmission();
        multipleWorkflowDefinitions();
        std::cout << "Workflow contracts passed: graph, fork/join, retry, timeout, cancellation, multiple definitions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
