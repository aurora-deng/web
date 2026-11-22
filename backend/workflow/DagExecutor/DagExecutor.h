#pragma once

#include "../WorkerPool/WorkerPool.h"
#include "../WorkflowGraph/WorkflowGraph.h"

#include <cstdint>

namespace nebula::workflow {

enum class State { pending, queued, started, success, failed, timeout, retry, skipped, cancelled };
enum class EventKind { run, edge, node };
std::string_view stateName(State state);

struct Event {
    EventKind kind;
    State state;
    std::string_view workflow;
    std::string_view runId;
    std::string_view traceId;
    std::string_view nodeId;
    std::string_view predecessor;
    unsigned attempt{};
    std::uint64_t occurredNs{};
    std::uint64_t sequence{};
    double durationMs{};
    std::size_t inputCount{};
    std::string_view errorCode;
};

struct NodeResult {
    std::string nodeId;
    State state{State::pending};
    unsigned attempts{};
    std::string value;
    std::string errorCode;
};

struct RunResult {
    State state{State::pending};
    std::string traceId;
    std::vector<NodeResult> nodes;
    std::size_t peakActive{};
    std::size_t peakReady{};
};

struct RunOptions {
    std::string runId;
    std::string traceId{};
    Milliseconds timeout{5000};
    std::stop_token stop{};
    // Called on the scheduling thread. The view is valid during the callback.
    // Use a bounded, short callback and perform I/O outside the scheduler.
    std::function<void(const Event&)> onEvent{};
};

class DagExecutor {
public:
    explicit DagExecutor(std::size_t workers);
    // An executor admits a single run at a time. Jobs are drained before
    // returning, even when a node ignores cancellation and completes late.
    RunResult run(const WorkflowGraph& graph, RunOptions options);

private:
    std::mutex admission_;
    WorkerPool workers_;
};
}
