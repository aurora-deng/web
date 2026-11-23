#include "DurableWorkflow.h"

#include <stdexcept>
#include <utility>

namespace nebula::workflow {
namespace {

std::string definitionOf(const WorkflowGraph& graph, std::string_view revision) {
    std::string result = graph.name() + ":" + std::string(revision);
    for (const auto& step : graph.steps()) {
        const auto& node = step.node;
        result += "\n" + node.id + ":" + std::to_string(static_cast<int>(node.effect)) + ":" +
                  std::to_string(node.attempts) + ":" + std::to_string(node.timeout.count()) + ":" +
                  std::to_string(node.backoff.count());
        for (const auto& parent : node.needs) result += ":" + parent;
    }
    return result;
}

WorkResult bounded(WorkResult result) {
    if (result.value.size() > 4096) return {WorkStatus::uncertain, {}, "output_too_large"};
    if (!result.errorCode.empty() && !validIdentifier(result.errorCode)) result.errorCode = "execution_failed";
    return result;
}

bool settled(WorkStatus status) { return status == WorkStatus::success || status == WorkStatus::failure; }

}

std::unique_lock<std::mutex> WorkflowJournal::claim() {
    auto lock = std::unique_lock(owner_, std::try_to_lock);
    if (!lock.owns_lock()) throw std::runtime_error("workflow journal is busy");
    return lock;
}

DurableWorkflow::DurableWorkflow(WorkflowJournal& journal, std::size_t workers)
    : journal_(journal), executor_(workers) {}

RunResult DurableWorkflow::run(const WorkflowGraph& graph, std::string_view revision, RunOptions options) {
    if (!validIdentifier(options.runId, 64) || !validIdentifier(revision) ||
        (!options.traceId.empty() && !validTraceId(options.traceId)) ||
        options.timeout < Milliseconds{1} || options.timeout > Milliseconds{600000}) {
        throw std::invalid_argument("invalid durable workflow identity or deadline");
    }
    auto lease = journal_.claim();
    const auto durableId = options.runId;
    options.traceId = journal_.open(durableId, definitionOf(graph, revision), options.traceId);
    options.runId = "attempt-" + newTraceId();
    const auto clock = options.now;

    std::vector<Node> wrapped;
    wrapped.reserve(graph.steps().size());
    for (const auto& step : graph.steps()) {
        auto node = step.node;
        const auto* original = &step.node;
        node.execute = [&, original](const NodeContext& context) -> WorkResult {
            const auto prior = journal_.node(durableId, original->id);
            if (prior.result) return *prior.result;
            if (prior.attempts && original->effect == Effect::external) {
                return {WorkStatus::uncertain, {}, "requires_reconciliation"};
            }
            if (prior.attempts >= original->attempts) return {WorkStatus::failure, {}, "persistent_attempts_exhausted"};

            const auto attempt = prior.attempts + 1;
            journal_.start(durableId, original->id, original->effect, attempt);
            const auto key = durableId + ":" + original->id;
            auto durableContext = context;
            durableContext.runId = durableId;
            durableContext.idempotencyKey = key;
            durableContext.attempt = attempt;
            const auto now = [&] { return clock ? clock() : Clock::now(); };
            if (context.stop.stop_requested() || now() >= context.deadline) {
                return {WorkStatus::uncertain, {}, "checkpoint_stopped"};
            }
            auto result = bounded(original->execute(durableContext));
            if (context.stop.stop_requested() || now() >= context.deadline) {
                return {WorkStatus::uncertain, {}, "checkpoint_outcome_unknown"};
            }
            if (settled(result.status)) {
                journal_.settle(durableId, original->id, result);
            } else if (result.status == WorkStatus::retryable && original->effect == Effect::external) {
                return {WorkStatus::uncertain, {}, "requires_reconciliation"};
            }
            return result;
        };
        wrapped.push_back(std::move(node));
    }
    const WorkflowGraph recovered(graph.name(), std::move(wrapped));
    return executor_.run(recovered, std::move(options));
}

void DurableWorkflow::reconcile(std::string_view run, std::string_view node, const WorkResult& verified) {
    auto lease = journal_.claim();
    const auto checked = bounded(verified);
    const auto record = journal_.node(run, node);
    if (!record.attempts || record.effect != Effect::external || record.result || !settled(checked.status)) {
        throw std::invalid_argument("expected an unresolved external operation and a verified result");
    }
    journal_.settle(run, node, checked);
}

}
