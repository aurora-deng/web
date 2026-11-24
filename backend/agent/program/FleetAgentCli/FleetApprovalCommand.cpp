#include "FleetApprovalCommand.h"

#include <iomanip>
#include <iostream>
#include <utility>

namespace nebula::agent::cli {
namespace {

std::string stageKey(const fleet::ReviewDraft& draft) {
    return draft.sourceTaskId + ":" + std::to_string(draft.sourceEpoch);
}

std::string stageRun(std::string_view traceId) {
    return "stage-" + std::string(traceId);
}

bool stageUncertain(std::string_view error) {
    return error == "review_outcome_unknown" || error == "invalid_review_response" || error == "review_failed";
}

void printReview(const ReviewResult& result, std::string_view traceId, std::string_view state = "error") {
    std::cout << "{\"proposalId\":" << std::quoted(traceId);
    if (result.review) {
        const auto& review = *result.review;
        std::cout << ",\"state\":" << std::quoted(fleet::name(review.state))
                  << ",\"sourceTaskId\":" << std::quoted(review.draft.sourceTaskId)
                  << ",\"sourceEpoch\":" << review.draft.sourceEpoch
                  << ",\"station\":\"" << (review.station == fleet::Station::alpha ? "alpha" : "beta")
                  << "\",\"createdAt\":" << review.createdAt << ",\"expiresAt\":" << review.expiresAt
                  << ",\"replacementTaskId\":" << std::quoted(review.replacementTaskId);
    } else {
        std::cout << ",\"state\":" << std::quoted(state) << ",\"errorCode\":" << std::quoted(result.errorCode);
    }
    std::cout << "}\n";
}

int perform(std::string_view node, workflow::RunOptions options,
            std::function<ReviewResult()> operation, std::optional<fleet::ReviewState> expected) {
    using namespace workflow;
    ReviewResult result{{}, "review_outcome_unknown"};
    const WorkflowGraph graph("fleet.agent.approval.v1", {{
        std::string(node), {}, [&](const NodeContext&) -> WorkResult {
            result = operation();
            if (!result.review) return {WorkStatus::failure, {}, std::string(result.errorCode)};
            if (expected && result.review->state != *expected) {
                return {WorkStatus::failure, {}, "review_" + std::string(fleet::name(result.review->state))};
            }
            return {};
        }, Effect::idempotent, Milliseconds{2400},
    }});
    DagExecutor executor{1};
    const auto run = executor.run(graph, std::move(options));
    // State may commit even if the response times out. Expose the immutable ID for inspection.
    if (run.state == State::timeout || run.state == State::cancelled) result = {{}, "review_outcome_unknown"};
    printReview(result, run.traceId);
    return run.state == State::success ? 0 : 2;
}

}

int stageReview(std::string_view address, const fleet::ReviewDraft& draft, workflow::RunOptions options,
                workflow::DurableWorkflow& durable) {
    using namespace workflow;
    FleetReviewGrpc client(address, fleet::wire::agentDraftEnv);
    options.traceId = draft.traceId;
    options.runId = stageRun(draft.traceId);
    std::optional<ReviewResult> observed;
    const auto key = stageKey(draft);
    WorkflowGraph graph("fleet.agent.stage.v2", {
        {"bind_stage", {}, [&](const NodeContext&) { return WorkResult{WorkStatus::success, key, {}}; }},
        {"stage_review", {"bind_stage"}, [&](const NodeContext& context) -> WorkResult {
            if (context.inputs.front().value != key) return {WorkStatus::failure, {}, "draft_mismatch"};
            observed = client.stage(draft);
            if (observed->review) return {};
            const auto error = observed->errorCode;
            return {stageUncertain(error) ? WorkStatus::uncertain : WorkStatus::failure, {}, std::string(error)};
        }, Effect::external, Milliseconds{2400}}
    });
    // The draft identity remains bound even if the first Stage RPC committed
    // but the client never received its reply.
    const auto input = std::to_string(draft.sourceTaskId.size()) + "." + draft.sourceTaskId + "." +
                       std::to_string(draft.sourceEpoch);
    const auto run = durable.run(graph, "stage2", std::move(options), input);
    if (run.state == State::success) {
        // The coordinator's idempotent Stage endpoint rechecks the current
        // draft credential and returns current state for settled replays.
        const auto result = observed ? std::move(*observed) : client.stage(draft);
        printReview(result, run.traceId);
        return result.review ? 0 : 2;
    }
    const auto& error = run.nodes.back().errorCode;
    printReview({{}, error.empty() ? "review_outcome_unknown" : std::string_view(error)}, run.traceId,
                run.state == State::uncertain ? "uncertain" : "error");
    return 2;
}

int reconcileStage(std::string_view address, std::string_view traceId, workflow::DurableWorkflow& durable,
                   workflow::WorkflowJournal& journal) {
    FleetReviewGrpc client(address, fleet::wire::agentReviewEnv);
    const auto result = client.inspect(traceId);
    if (!result.review) {
        printReview(result, traceId);
        return 2;
    }
    const auto runId = stageRun(traceId);
    const auto bound = journal.node(runId, "bind_stage");
    if (!bound.result || bound.result->value != stageKey(result.review->draft)) {
        printReview({{}, "draft_mismatch"}, traceId);
        return 2;
    }
    durable.reconcile(runId, "stage_review", {});
    printReview(result, traceId);
    return 0;
}

int review(std::string_view address, std::string_view action, workflow::RunOptions options) {
    FleetReviewGrpc client(address, fleet::wire::agentReviewEnv);
    const auto traceId = options.traceId;
    if (action == "inspect") {
        return perform("inspect_review", std::move(options), [&] { return client.inspect(traceId); }, std::nullopt);
    }
    const auto approve = action == "approve";
    return perform(approve ? "approve_review" : "reject_review", std::move(options),
                   [&] { return client.decide(traceId, approve ? fleet::Decision::approve : fleet::Decision::reject); },
                   approve ? fleet::ReviewState::approved : fleet::ReviewState::rejected);
}

}
