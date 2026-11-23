#pragma once

#include "../SkillCatalog/SkillCatalog.h"
#include "backend/fleet/domain/FleetTypes/FleetTypes.h"

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
};

struct Proposal {
    std::string skillId;
    unsigned version{};
    std::vector<std::string> evidenceTaskIds;
    bool operator==(const Proposal&) const = default;
};

bool validRequest(const Request& request);
bool validEvidence(const TaskEvidence& evidence, std::string_view requestedId);
std::optional<Proposal> allowedProposal(Intent intent, std::span<const TaskEvidence> evidence);

}
