#include "FleetAgent.h"
#include "AgentRecords.h"

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

FleetAgent::FleetAgent(TaskEvidenceReader& reader, DurableWorkflow& durable, ProposalProvider* provider,
                       Milliseconds planningBudget)
    : reader_(reader), provider_(provider), durable_(&durable), planningBudget_(planningBudget) {
    if (planningBudget_ < Milliseconds{1} || planningBudget_ > Milliseconds{30000}) {
        throw std::invalid_argument("invalid planner budget");
    }
}

Answer FleetAgent::run(const Request& request, RunOptions options) {
    const WorkflowGraph graph("fleet.agent.proposal.v2", {
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
            return WorkResult{WorkStatus::success, records::evidenceOf(batch), {}};
        }, Effect::read, Milliseconds{600}, 2},
        {"build_proposal", {"read_task_status"}, [&](const NodeContext& context) {
            const auto evidence = records::evidenceFrom(context.inputs.front().value);
            const auto permitted = allowedProposal(request.intent, evidence);
            if (!permitted) {
                return WorkResult{WorkStatus::failure, {}, request.intent == Intent::review ?
                                  "no_failure_evidence" : "task_not_eligible"};
            }
            if (!provider_) {
                return WorkResult{WorkStatus::success, records::proposalOf(PlannerSource::rules, *permitted), {}};
            }
            const auto deadline = std::min(context.deadline, Clock::now() + planningBudget_);
            auto provided = provider_->plan(request.intent, evidence, context.stop, deadline);
            if (context.stop.stop_requested()) return WorkResult{WorkStatus::failure, {}, "stopped"};
            if (provided.status == CandidateStatus::unavailable || Clock::now() >= deadline) {
                return WorkResult{WorkStatus::success, records::proposalOf(PlannerSource::fallback, *permitted), {}};
            }
            if (provided.status != CandidateStatus::proposed) {
                return WorkResult{WorkStatus::failure, {}, "provider_malformed"};
            }
            return WorkResult{WorkStatus::success, records::proposalOf(PlannerSource::provider, provided.proposal), {}};
        }, Effect::read, planningBudget_ + Milliseconds{300}},
        {"verify_proposal", {"read_task_status", "build_proposal"}, [&](const NodeContext& context) {
            const auto evidence = records::evidenceFrom(context.inputs[0].value);
            const auto permitted = allowedProposal(request.intent, evidence);
            const auto [source, candidate] = records::proposalFrom(context.inputs[1].value);
            const auto* skill = findSkill(candidate.skillId, candidate.version);
            const bool permittedByEvidence = permitted && candidate == *permitted;
            const bool registeredProposal = skill && skill->effect == SkillEffect::proposal;
            if (!registeredProposal || !permittedByEvidence) {
                return WorkResult{WorkStatus::failure, {}, "proposal_rejected"};
            }
            return WorkResult{};
        }},
    });

    std::string inputIdentity;
    if (validRequest(request)) {
        inputIdentity = request.intent == Intent::review ? "review" : "reschedule";
        // Length prefixes keep distinct task lists distinct when IDs contain dots.
        for (const auto& id : request.taskIds) inputIdentity += "." + std::to_string(id.size()) + "." + id;
    } else {
        inputIdentity = "invalid";
    }
    inputIdentity += "." + (provider_ ? provider_->checkpointIdentity() : std::string("rules"));
    auto run = durable_ ? durable_->run(graph, "proposal2", std::move(options), inputIdentity)
                        : executor_.run(graph, std::move(options));
    const auto read = std::ranges::find(run.nodes, "read_task_status", &NodeResult::nodeId);
    auto evidence = read->state == State::success ? records::evidenceFrom(read->value) : std::vector<TaskEvidence>{};
    const auto planned = std::ranges::find(run.nodes, "build_proposal", &NodeResult::nodeId);
    auto [source, candidate] = planned->state == State::success ? records::proposalFrom(planned->value)
                                                              : std::pair{PlannerSource::none, Proposal{}};
    auto verified = run.state == State::success ? std::optional(std::move(candidate)) : std::nullopt;
    return {std::move(run), std::move(evidence), std::move(verified), source};
}

}
