#pragma once

#include "../../domain/FleetProposal/FleetProposal.h"

#include <chrono>
#include <stop_token>

namespace nebula::agent {

enum class ReadStatus { found, missing, denied, unavailable, invalid };

struct EvidenceResult {
    ReadStatus status{ReadStatus::invalid};
    TaskEvidence evidence;
};

class TaskEvidenceReader {
public:
    virtual ~TaskEvidenceReader() = default;
    virtual EvidenceResult read(std::string_view taskId, std::stop_token stop,
                                std::chrono::steady_clock::time_point deadline) = 0;
};

}
