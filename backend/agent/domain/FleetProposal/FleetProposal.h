#pragma once

#include "../SkillCatalog/SkillCatalog.h"
#include "backend/fleet/domain/FleetTypes/FleetTypes.h"

#include <chrono>
#include <optional>
#include <vector>

namespace nebula::agent {

enum class Intent { review, simulateReschedule };
inline constexpr std::size_t maxReviewTasks = 3;

struct Request {
    Intent intent;
    std::vector<std::string> taskIds;
};

struct TaskEvidence {
    std::string taskId;
    std::string traceId;
    fleet::Phase phase{};
    std::int64_t epoch{};
    bool assigned{};
    bool needsReconciliation{};
    std::string outcome;
    // Set by the trusted reader, never a provider or a user-supplied request.
    std::string source;
    std::chrono::system_clock::time_point readAt{};
    bool fresh{false};
};

struct Proposal {
    std::string skillId;
    unsigned version{};
    std::vector<std::string> evidenceTaskIds;
    Arguments parameters;
    bool operator==(const Proposal&) const;
};

Proposal fleetProposal(std::string_view skill, unsigned version, std::vector<std::string> evidence);
bool validRequest(const Request& request);
bool validEvidence(const TaskEvidence& evidence, std::string_view requestedId);
std::optional<Proposal> allowedProposal(Intent intent, std::span<const TaskEvidence> evidence);

}
