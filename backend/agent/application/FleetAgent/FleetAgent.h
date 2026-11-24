#pragma once

#include "../../ports/ProposalProvider/ProposalProvider.h"
#include "../../ports/TaskEvidenceReader/TaskEvidenceReader.h"
#include "backend/workflow/DagExecutor/DagExecutor.h"
#include "backend/workflow/DurableWorkflow/DurableWorkflow.h"

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
    FleetAgent(TaskEvidenceReader& reader, workflow::DurableWorkflow& durable, ProposalProvider* provider = nullptr,
               workflow::Milliseconds planningBudget = workflow::Milliseconds{2000});
    Answer run(const Request& request, workflow::RunOptions options);

private:
    TaskEvidenceReader& reader_;
    ProposalProvider* provider_;
    workflow::DurableWorkflow* durable_{};
    workflow::Milliseconds planningBudget_{200};
    workflow::DagExecutor executor_{1};
};

}
