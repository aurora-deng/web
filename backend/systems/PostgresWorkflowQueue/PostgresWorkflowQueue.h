#pragma once

#include "backend/workflow/WorkflowGraph/WorkflowGraph.h"

#include <memory>
#include <optional>

namespace nebula::systems {

struct NodeLease {
    std::string run;
    std::string node;
    std::string owner;
    std::string trace;
    unsigned attempt{};
    workflow::Milliseconds remaining{};
    std::vector<workflow::DependencyValue> inputs;
    // The dependency views refer to Graph node IDs supplied to claim().
};

struct ReadyWorkflow {
    std::string run;
    std::string workflow;
};

class PostgresWorkflowQueue {
public:
    explicit PostgresWorkflowQueue(std::string_view dsn);
    ~PostgresWorkflowQueue();

    std::string submit(std::string_view run, const workflow::WorkflowGraph& graph, workflow::Milliseconds budget);
    std::vector<ReadyWorkflow> readyRuns(std::span<const workflow::WorkflowGraph> registered);
    std::optional<NodeLease> claim(std::string_view run, const workflow::WorkflowGraph& graph,
                                   std::string_view owner, workflow::Milliseconds lease);
    bool renew(const NodeLease& lease, workflow::Milliseconds extension);
    bool finish(const NodeLease& lease, const workflow::WorkResult& result);
    bool reconcile(std::string_view run, std::string_view node, unsigned attempt,
                   bool succeeded, std::string_view evidenceRef, std::string_view value);
    std::string state(std::string_view run);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nebula::systems
