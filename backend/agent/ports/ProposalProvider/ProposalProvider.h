#pragma once

#include "../../domain/FleetProposal/FleetProposal.h"

#include <chrono>
#include <stop_token>

namespace nebula::agent {

enum class CandidateStatus { proposed, unavailable, malformed };

struct Candidate {
    CandidateStatus status{CandidateStatus::malformed};
    Proposal proposal;
};

class ProposalProvider {
public:
    virtual ~ProposalProvider() = default;
    virtual std::string checkpointIdentity() const { return "provider"; }
    // Return by deadline and honor stop. Supply typed values from authorized evidence only.
    virtual Candidate plan(Intent intent, std::span<const TaskEvidence> evidence, std::stop_token stop,
                           std::chrono::steady_clock::time_point deadline) = 0;
};

}
