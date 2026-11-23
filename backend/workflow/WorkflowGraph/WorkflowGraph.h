#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace nebula::workflow {
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

enum class Effect { read, idempotent, external };
enum class WorkStatus { success, failure, retryable, uncertain };

struct WorkResult {
    WorkStatus status{WorkStatus::success};
    std::string value;
    std::string errorCode;
};

struct DependencyValue {
    std::string_view nodeId;
    std::string value;
};

struct NodeContext {
    std::string_view runId;
    std::string_view traceId;
    // Stable across attempts. Treat the run and node identity as an opaque key.
    std::string_view idempotencyKey;
    unsigned attempt;
    Clock::time_point deadline;
    std::stop_token stop;
    std::span<const DependencyValue> inputs;
};

struct Node {
    std::string id;
    std::vector<std::string> needs;
    std::function<WorkResult(const NodeContext&)> execute;
    Effect effect{Effect::read};
    Milliseconds timeout{1000};
    unsigned attempts{1};
    Milliseconds backoff{20};
};

bool validIdentifier(std::string_view id, std::size_t maxLength = 80);
bool validTraceId(std::string_view traceId);
std::string newTraceId();

class WorkflowGraph {
public:
    struct Step {
        Node node;
        std::vector<std::size_t> dependencies;
    };

    WorkflowGraph(std::string name, std::vector<Node> nodes);
    const std::string& name() const noexcept { return name_; }
    const std::vector<Step>& steps() const noexcept { return steps_; }

private:
    std::string name_;
    std::vector<Step> steps_;
};
}
