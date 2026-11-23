#pragma once

#include "../../ports/ProposalProvider/ProposalProvider.h"
#include "../../ports/TaskEvidenceReader/TaskEvidenceReader.h"
#include "backend/workflow/DagExecutor/DagExecutor.h"

namespace nebula::agent {

enum class PlannerSource { none, rules, provider, fallback };
std::string_view plannerName(PlannerSource source);

struct Answer {
    workflow::RunResult run;
    std::vector<TaskEvidence> evidence;
    std::optional<Proposal> proposal;
    PlannerSource planner{PlannerSource::none};
};

class FleetAgent {
public:
    explicit FleetAgent(TaskEvidenceReader& reader, ProposalProvider* provider = nullptr);
    Answer run(const Request& request, workflow::RunOptions options);

private:
    TaskEvidenceReader& reader_;
    ProposalProvider* provider_;
    workflow::DagExecutor executor_{1};
};

}
