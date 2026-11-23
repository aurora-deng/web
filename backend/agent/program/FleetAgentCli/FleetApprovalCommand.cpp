#include "FleetApprovalCommand.h"

#include <iomanip>
#include <iostream>
#include <utility>

namespace nebula::agent::cli {
namespace {

void printReview(const ReviewResult& result, std::string_view traceId) {
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
        std::cout << ",\"state\":\"error\",\"errorCode\":" << std::quoted(result.errorCode);
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

int stageReview(std::string_view address, const fleet::ReviewDraft& draft, workflow::RunOptions options) {
    FleetReviewGrpc client(address, fleet::wire::agentDraftEnv);
    options.traceId = draft.traceId;
    return perform("stage_review", std::move(options), [&] { return client.stage(draft); }, std::nullopt);
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
