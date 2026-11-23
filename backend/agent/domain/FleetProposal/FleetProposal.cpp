#include "FleetProposal.h"
#include "backend/fleet/domain/SimulationReview/SimulationReview.h"

#include <algorithm>
#include <iterator>

namespace nebula::agent {

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
        return Proposal{std::string(proposeSimReschedule.id), proposeSimReschedule.version, {task.taskId}};
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
    return review;
}

}
