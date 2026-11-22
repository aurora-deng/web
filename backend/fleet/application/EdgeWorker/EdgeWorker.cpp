#include "EdgeWorker.h"

#include <utility>

namespace nebula::fleet {

EdgeWorker::EdgeWorker(EdgeLedger& ledger, TaskWorkflow& task, std::function<void(const workflow::Event&)> observe,
                       std::function<void()> flush)
    : ledger_(ledger), task_(task), observe_(std::move(observe)), flush_(std::move(flush)),
      worker_([this](std::stop_token stop) { work(stop); }) {}

EdgeWorker::~EdgeWorker() = default;

void EdgeWorker::permit() {
    {
        std::lock_guard lock(mutex_);
        permitUntil_ = workflow::Clock::now() + workflow::Milliseconds{150};
    }
    signal_.notify_one();
}

void EdgeWorker::work(std::stop_token stop) {
    try {
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                if (!signal_.wait(lock, stop, [&] { return permitUntil_ != workflow::Clock::time_point{}; })) return;
                const auto validUntil = std::exchange(permitUntil_, {});
                if (workflow::Clock::now() >= validUntil) continue;
            }
            if (stop.stop_requested()) return;
            if (auto command = ledger_.beginReady()) {
                const auto outcome = task_.run(*command, stop, observe_);
                ledger_.finish(*command, outcome.phase, outcome.code);
                if (flush_) {
                    try { flush_(); }
                    catch (...) { /* Telemetry is best effort after the durable outcome. */ }
                }
            }
        }
    } catch (...) {
        // The executing checkpoint is intentionally kept for startup recovery.
        healthy_.store(false);
    }
}
}
