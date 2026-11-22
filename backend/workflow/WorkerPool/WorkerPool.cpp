#include "WorkerPool.h"

#include <stdexcept>
#include <utility>

namespace nebula::workflow {

WorkerPool::WorkerPool(std::size_t workers) {
    if (workers < 1 || workers > 16) throw std::invalid_argument("worker count must be 1-16");
    workers_.reserve(workers);
    try {
        for (std::size_t i = 0; i < workers; ++i) workers_.emplace_back([this] { work(); });
    } catch (...) {
        close();
        throw;
    }
}

WorkerPool::~WorkerPool() { close(); }

void WorkerPool::close() noexcept {
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
    }
    ready_.notify_all();
}

void WorkerPool::submit(std::function<void()> workItem) {
    {
        std::lock_guard lock(mutex_);
        if (closing_ || jobs_.size() >= capacity()) throw std::logic_error("workflow worker queue is full");
        jobs_.push_back(std::move(workItem));
    }
    ready_.notify_one();
}

void WorkerPool::work() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [&] { return closing_ || !jobs_.empty(); });
            if (jobs_.empty()) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }
}
}
