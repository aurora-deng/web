#include "FleetAgent.h"

#include <algorithm>
#include <utility>

namespace nebula::agent {
using namespace workflow;

namespace {

WorkResult readFailure(ReadStatus status) {
    switch (status) {
    case ReadStatus::missing: return {WorkStatus::failure, {}, "unknown_task"};
    case ReadStatus::denied: return {WorkStatus::failure, {}, "evidence_denied"};
    case ReadStatus::unavailable: return {WorkStatus::retryable, {}, "evidence_unavailable"};
    default: return {WorkStatus::failure, {}, "invalid_evidence"};
    }
}

}

std::string_view plannerName(PlannerSource source) {
    switch (source) {
    case PlannerSource::rules: return "rules";
    case PlannerSource::provider: return "provider";
    case PlannerSource::fallback: return "rules_fallback";
    default: return "none";
    }
}

FleetAgent::FleetAgent(TaskEvidenceReader& reader, ProposalProvider* provider)
    : reader_(reader), provider_(provider) {}

Answer FleetAgent::run(const Request& request, RunOptions options) {
    std::vector<TaskEvidence> evidence;
    std::optional<Proposal> permitted;
    Proposal candidate;
    auto planner = PlannerSource::none;

    // Each buffer is handed to the next node only after the owning node completes.
    // The executor drains all admitted jobs before returning these stack variables.
    const WorkflowGraph graph("fleet.agent.proposal.v1", {
        {"validate_request", {}, [&](const NodeContext&) {
            return validRequest(request) ? WorkResult{} : WorkResult{WorkStatus::failure, {}, "invalid_request"};
        }},
        {"read_task_status", {"validate_request"}, [&](const NodeContext& context) {
            std::vector<TaskEvidence> batch;
            batch.reserve(request.taskIds.size());
            for (const auto& taskId : request.taskIds) {
                if (context.stop.stop_requested()) return WorkResult{WorkStatus::failure, {}, "stopped"};
                auto result = reader_.read(taskId, context.stop, context.deadline);
                if (result.status != ReadStatus::found) return readFailure(result.status);
                if (!validEvidence(result.evidence, taskId)) return readFailure(ReadStatus::invalid);
                batch.push_back(std::move(result.evidence));
            }
            evidence = std::move(batch);
            return WorkResult{};
        }, Effect::read, Milliseconds{600}, 2},
        {"build_proposal", {"read_task_status"}, [&](const NodeContext& context) {
            permitted = allowedProposal(request.intent, evidence);
            if (!permitted) {
                return WorkResult{WorkStatus::failure, {}, request.intent == Intent::review ?
                                  "no_failure_evidence" : "task_not_eligible"};
            }
            if (!provider_) {
                planner = PlannerSource::rules;
                candidate = *permitted;
                return WorkResult{};
            }
            const auto deadline = std::min(context.deadline, Clock::now() + Milliseconds{200});
            auto provided = provider_->plan(request.intent, evidence, context.stop, deadline);
            if (context.stop.stop_requested()) return WorkResult{WorkStatus::failure, {}, "stopped"};
            if (provided.status == CandidateStatus::unavailable || Clock::now() >= deadline) {
                planner = PlannerSource::fallback;
                candidate = *permitted;
                return WorkResult{};
            }
            if (provided.status != CandidateStatus::proposed) {
                return WorkResult{WorkStatus::failure, {}, "provider_malformed"};
            }
            planner = PlannerSource::provider;
            candidate = std::move(provided.proposal);
            return WorkResult{};
        }, Effect::read, Milliseconds{500}},
        {"verify_proposal", {"build_proposal"}, [&](const NodeContext&) {
            const auto* skill = findSkill(candidate.skillId, candidate.version);
            if (!skill || skill->effect != SkillEffect::proposal || candidate != *permitted) {
                return WorkResult{WorkStatus::failure, {}, "proposal_rejected"};
            }
            return WorkResult{};
        }},
    });

    auto run = executor_.run(graph, std::move(options));
    const auto read = std::ranges::find(run.nodes, "read_task_status", &NodeResult::nodeId);
    if (read->state != State::success) evidence.clear();
    std::optional<Proposal> verified;
    if (run.state == State::success) verified = std::move(candidate);
    return {std::move(run), std::move(evidence), std::move(verified), planner};
}

}
