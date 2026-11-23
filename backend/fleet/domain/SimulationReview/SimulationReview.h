#pragma once

#include "../FleetTypes/FleetTypes.h"

namespace nebula::fleet {

inline constexpr std::string_view replacementPrefix = "agent-approved-";
inline constexpr std::string_view supersededOutcome = "agent_superseded";
inline constexpr std::int64_t reviewLifetimeSeconds = 300;

enum class ReviewState : int { pending = 1, approved, rejected, stale, expired };
enum class Decision : int { approve = 1, reject };

struct ReviewDraft {
    std::string traceId;
    std::string sourceTaskId;
    std::int64_t sourceEpoch{};
    bool operator==(const ReviewDraft&) const = default;
};

struct SimulationReview {
    ReviewDraft draft;
    Station station;
    ReviewState state;
    std::int64_t createdAt;
    std::int64_t expiresAt;
    std::string replacementTaskId;
};

inline std::string_view name(ReviewState state) {
    switch (state) {
    case ReviewState::pending: return "pending";
    case ReviewState::approved: return "approved";
    case ReviewState::rejected: return "rejected";
    case ReviewState::stale: return "stale";
    case ReviewState::expired: return "expired";
    }
    return "invalid";
}

}
