#include "backend/workflow/DagExecutor/DagExecutor.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace nebula::workflow;

namespace {

std::uint64_t residentPeak() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS memory{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) throw std::runtime_error("RSS sample failed");
    return memory.PeakWorkingSetSize;
#else
    rusage resources{};
    if (getrusage(RUSAGE_SELF, &resources) != 0) throw std::runtime_error("RSS sample failed");
    return static_cast<std::uint64_t>(resources.ru_maxrss) * 1024;
#endif
}

double percentile(const std::vector<double>& values, double ratio) {
    return values[static_cast<std::size_t>(std::ceil(values.size() * ratio)) - 1];
}

WorkflowGraph fixture() {
    const auto branch = [](const NodeContext& context) {
        std::uint64_t value = context.runId.back();
        for (unsigned i = 0; i < 50000; ++i) value = (value ^ (value >> 13)) * 1099511628211ULL + i;
        return WorkResult{WorkStatus::success, std::to_string(value), {}};
    };
    return WorkflowGraph("bench.fork_join.v1", {
        {"a", {}, [](const NodeContext&) { return WorkResult{}; }},
        {"b", {"a"}, branch}, {"c", {"a"}, branch},
        {"d", {"b", "c"}, [](const NodeContext& node) {
            return WorkResult{WorkStatus::success, node.inputs.front().value + node.inputs.back().value, {}};
        }},
        {"e", {"d"}, [](const NodeContext&) { return WorkResult{}; }},
    });
}

}

int main() {
    try {
        const auto graph = fixture();
        std::cout << "workers,round,operations,p50_ms,p95_ms,p99_ms,peak_active,peak_ready,process_peak_rss_bytes\n";
        for (unsigned workers : {1U, 2U, 4U}) {
            DagExecutor executor(workers);
            for (unsigned round = 1; round <= 3; ++round) {
                std::vector<double> latencies;
                std::size_t peakActive = 0, peakReady = 0;
                for (unsigned job = 0; job < 200; ++job) {
                    const auto start = Clock::now();
                    const auto run = executor.run(graph, {.runId = "job-" + std::to_string(job)});
                    latencies.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
                    if (run.state != State::success || run.peakActive > workers) throw std::runtime_error("benchmark graph failed");
                    peakActive = std::max(peakActive, run.peakActive);
                    peakReady = std::max(peakReady, run.peakReady);
                }
                std::ranges::sort(latencies);
                std::cout << workers << ',' << round << ',' << latencies.size() << ',' << percentile(latencies, 0.5) << ','
                          << percentile(latencies, 0.95) << ',' << percentile(latencies, 0.99) << ','
                          << peakActive << ',' << peakReady << ',' << residentPeak() << '\n';
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
