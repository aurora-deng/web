#pragma once

#include "../../ports/FleetLedger/FleetLedger.h"
#include "../TaskWorkflow/TaskWorkflow.h"

#include <atomic>
#include <condition_variable>
#include <thread>

namespace nebula::fleet {

class EdgeWorker {
public:
    EdgeWorker(EdgeLedger& ledger, TaskWorkflow& task, std::function<void(const workflow::Event&)> observe,
               std::function<void()> flush = {});
    ~EdgeWorker();

    // Refresh only after a successful exchange. At most one permit is held.
    void permit();
    bool healthy() const noexcept { return healthy_.load(); }

private:
    void work(std::stop_token stop);

    EdgeLedger& ledger_;
    TaskWorkflow& task_;
    std::function<void(const workflow::Event&)> observe_;
    std::function<void()> flush_;
    std::mutex mutex_;
    std::condition_variable_any signal_;
    workflow::Clock::time_point permitUntil_{};
    std::atomic_bool healthy_{true};
    std::jthread worker_;
};
}
