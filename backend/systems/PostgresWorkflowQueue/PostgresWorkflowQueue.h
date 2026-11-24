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

class PostgresWorkflowQueue {
public:
    explicit PostgresWorkflowQueue(std::string_view dsn);
    ~PostgresWorkflowQueue();

    std::string submit(std::string_view run, const workflow::WorkflowGraph& graph, workflow::Milliseconds budget);
    std::optional<NodeLease> claim(std::string_view run, const workflow::WorkflowGraph& graph,
                                   std::string_view owner, workflow::Milliseconds lease);
    bool finish(const NodeLease& lease, const workflow::WorkResult& result);
    std::string state(std::string_view run);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nebula::systems
