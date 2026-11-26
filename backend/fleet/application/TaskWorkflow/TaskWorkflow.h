#pragma once

#include "../../ports/NavigationSkill/NavigationSkill.h"
#include "backend/workflow/DurableWorkflow/DurableWorkflow.h"
#include <memory>

namespace nebula::fleet {

struct Outcome {
    Phase phase;
    std::string code;
};

class TaskWorkflow {
public:
    // Volatile executor is reserved for isolated unit tests. Production uses the journal overload.
    explicit TaskWorkflow(NavigationSkill& navigation, workflow::Milliseconds navigationTimeout);
    TaskWorkflow(NavigationSkill& navigation, workflow::Milliseconds navigationTimeout, workflow::WorkflowJournal& journal);
    Outcome run(const Command& command, std::stop_token stop, std::function<void(const workflow::Event&)> sink);

private:
    NavigationSkill& navigation_;
    workflow::Milliseconds navigationTimeout_;
    workflow::DagExecutor executor_{2};
    std::unique_ptr<workflow::DurableWorkflow> durable_;
};
}
