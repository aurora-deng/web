#include "FleetProposal.h"
#include "backend/fleet/domain/SimulationReview/SimulationReview.h"

#include <algorithm>
#include <iterator>

namespace nebula::agent {

namespace {
std::vector<std::string> ordered(std::vector<std::string> ids) { std::ranges::sort(ids); return ids; }
Arguments normalized(const Proposal& proposal) {
    auto args=proposal.parameters;
    if (proposal.skillId==reviewFailures.id && args.contains("taskIds")) {
        if (auto* ids=std::get_if<ParameterValue::Array>(&args.at("taskIds").value)) {
            if (std::ranges::all_of(*ids,[](const auto& id) { return std::holds_alternative<std::string>(id.value); }))
                std::ranges::sort(*ids,{},[](const auto& id) { return std::get<std::string>(id.value); });
        }
    }
    return args;
}
}
bool Proposal::operator==(const Proposal& other) const {
    return skillId==other.skillId && version==other.version && ordered(evidenceTaskIds)==ordered(other.evidenceTaskIds) &&
        normalized(*this)==normalized(other);
}
Proposal fleetProposal(std::string_view skill,unsigned version,std::vector<std::string> evidence) {
    Proposal p{std::string(skill),version,std::move(evidence),{}};
    if (skill==reviewFailures.id) {
        ParameterValue::Array ids; for (const auto& id:p.evidenceTaskIds) ids.emplace_back(id);
        p.parameters.emplace("taskIds",std::move(ids));
    } else if (skill==proposeSimReschedule.id && p.evidenceTaskIds.size()==1) {
        p.parameters.emplace("taskId",p.evidenceTaskIds.front());
    }
    return p;
}

bool validRequest(const Request& request) {
    if (request.taskIds.empty() || request.taskIds.size() > maxReviewTasks ||
        (request.intent != Intent::review && request.intent != Intent::simulateReschedule) ||
        (request.intent == Intent::simulateReschedule && request.taskIds.size() != 1)) return false;
    for (auto id = request.taskIds.begin(); id != request.taskIds.end(); ++id) {
        if (!fleet::validId(*id) || std::find(std::next(id), request.taskIds.end(), *id) != request.taskIds.end()) {
            return false;
        }
    }
    return true;
}

bool validEvidence(const TaskEvidence& evidence, std::string_view requestedId) {
    using fleet::Phase;
    const auto phase = evidence.phase;
    if (evidence.source.empty() || evidence.source.size() > 64 ||
        !std::ranges::all_of(evidence.source, [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        }) || evidence.readAt <= std::chrono::system_clock::time_point{} ||
        evidence.readAt > std::chrono::system_clock::now()) return false;
    const bool terminal = phase == Phase::succeeded || phase == Phase::failed || phase == Phase::uncertain;
    if (evidence.taskId != requestedId || !fleet::validId(evidence.taskId) ||
        !fleet::validTrace(evidence.traceId) || evidence.epoch <= 0 || fleet::name(phase) == "invalid") return false;
    if (phase == Phase::queued && (evidence.assigned || evidence.needsReconciliation)) return false;
    const bool superseded = phase == Phase::failed && evidence.outcome == fleet::supersededOutcome;
    if (superseded) return !evidence.assigned && !evidence.needsReconciliation;
    if (phase != Phase::queued && !evidence.assigned) return false;
    if ((phase == Phase::uncertain && !evidence.needsReconciliation) ||
        (evidence.needsReconciliation && !fleet::active(phase))) return false;
    return terminal ? fleet::validId(evidence.outcome) : evidence.outcome.empty();
}

std::optional<Proposal> allowedProposal(Intent intent, std::span<const TaskEvidence> evidence) {
    using fleet::Phase;
    if (intent == Intent::simulateReschedule) {
        if (evidence.size() != 1) return std::nullopt;
        const auto& task = evidence.front();
        if (task.phase != Phase::queued || task.assigned || task.needsReconciliation ||
            task.taskId.starts_with(fleet::replacementPrefix)) return std::nullopt;
        return fleetProposal(proposeSimReschedule.id, proposeSimReschedule.version, {task.taskId});
    }
    if (intent != Intent::review) return std::nullopt;
    Proposal review{std::string(reviewFailures.id), reviewFailures.version, {}};
    for (const auto& task : evidence) {
        if ((task.phase == Phase::failed && task.outcome != fleet::supersededOutcome) ||
            task.phase == Phase::uncertain || task.needsReconciliation) {
            review.evidenceTaskIds.push_back(task.taskId);
        }
    }
    if (review.evidenceTaskIds.empty()) return std::nullopt;
    return fleetProposal(review.skillId, review.version, std::move(review.evidenceTaskIds));
}

}
