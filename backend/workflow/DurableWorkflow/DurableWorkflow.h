#pragma once

#include "../DagExecutor/DagExecutor.h"
#include "../WorkflowJournal/WorkflowJournal.h"

namespace nebula::workflow {

class DurableWorkflow {
public:
    DurableWorkflow(WorkflowJournal& journal, std::size_t workers);
    // revision identifies immutable inputs and implementation, including changes invisible to graph topology.
    RunResult run(const WorkflowGraph& graph, std::string_view revision, RunOptions options);
    void reconcile(std::string_view run, std::string_view node, const WorkResult& verified);

private:
    WorkflowJournal& journal_;
    DagExecutor executor_;
};

}
