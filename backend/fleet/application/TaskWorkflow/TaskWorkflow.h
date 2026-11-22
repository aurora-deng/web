#pragma once

#include "../../ports/NavigationSkill/NavigationSkill.h"
#include "backend/workflow/DagExecutor/DagExecutor.h"

namespace nebula::fleet {

struct Outcome {
    Phase phase;
    std::string code;
};

class TaskWorkflow {
public:
    explicit TaskWorkflow(NavigationSkill& navigation, workflow::Milliseconds navigationTimeout);
    Outcome run(const Command& command, std::stop_token stop, std::function<void(const workflow::Event&)> sink);

private:
    NavigationSkill& navigation_;
    workflow::Milliseconds navigationTimeout_;
    workflow::DagExecutor executor_{2};
};
}
