#include "WorkflowGraph.h"

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <utility>

namespace nebula::workflow {

bool validIdentifier(std::string_view id, std::size_t maxLength) {
    return !id.empty() && id.size() <= maxLength && std::ranges::all_of(id, [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
               ch == '_' || ch == '-' || ch == '.';
    });
}

bool validTraceId(std::string_view traceId) {
    return traceId.size() == 32 && traceId.find_first_not_of('0') != std::string_view::npos &&
           std::ranges::all_of(traceId, [](char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}

std::string newTraceId() {
    static thread_local auto random = [] {
        std::random_device entropy;
        std::seed_seq seed{entropy(), entropy(), entropy(), entropy()};
        return std::mt19937_64(seed);
    }();
    constexpr auto hex = "0123456789abcdef";
    std::string id(32, '0');
    for (std::size_t offset : {0U, 16U}) {
        auto word = random();
        for (std::size_t nibble = 0; nibble < 16; ++nibble) {
            id[offset + nibble] = hex[word & 15U];
            word >>= 4;
        }
    }
    if (!validTraceId(id)) id.back() = '1';
    return id;
}

WorkflowGraph::WorkflowGraph(std::string name, std::vector<Node> nodes) : name_(std::move(name)) {
    if (!validIdentifier(name_) || nodes.empty() || nodes.size() > 128) {
        throw std::invalid_argument("invalid workflow name or size");
    }
    std::map<std::string, Node, std::less<>> byName;
    std::size_t edges = 0;
    for (auto& node : nodes) {
        edges += node.needs.size();
        const bool validRetry = node.attempts >= 1 && node.attempts <= 3 &&
                                (node.effect != Effect::external || node.attempts == 1);
        if (!validIdentifier(node.id) || !node.execute || !validRetry ||
            node.timeout < Milliseconds{1} || node.timeout > Milliseconds{600000} ||
            node.backoff < Milliseconds{0} || node.backoff > Milliseconds{5000} ||
            edges > 1024 || std::set(node.needs.begin(), node.needs.end()).size() != node.needs.size()) {
            throw std::invalid_argument("invalid workflow node: " + node.id);
        }
        if (!byName.emplace(node.id, std::move(node)).second) {
            throw std::invalid_argument("duplicate workflow node");
        }
    }
    std::map<std::string, std::size_t, std::less<>> completed;
    std::set<std::string, std::less<>> visiting;
    steps_.reserve(nodes.size());
    const auto visit = [&](const auto& recurse, const std::string& id) -> std::size_t {
        if (const auto found = completed.find(id); found != completed.end()) return found->second;
        const auto found = byName.find(id);
        if (found == byName.end()) throw std::invalid_argument("missing workflow predecessor: " + id);
        if (!visiting.insert(id).second) throw std::invalid_argument("workflow cycle at: " + id);
        std::vector<std::size_t> dependencies;
        for (const auto& dependency : found->second.needs) {
            dependencies.push_back(recurse(recurse, dependency));
        }
        visiting.erase(id);
        const auto index = steps_.size();
        steps_.push_back({std::move(found->second), std::move(dependencies)});
        completed.emplace(id, index);
        return index;
    };
    for (const auto& [id, node] : byName) visit(visit, id);
}
}
