#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace nebula::workflow {
class WorkerPool {
public:
    explicit WorkerPool(std::size_t workers);
    ~WorkerPool();

    void submit(std::function<void()> work);
    std::size_t capacity() const noexcept { return workers_.size(); }

private:
    void work();
    void close() noexcept;

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> jobs_;
    bool closing_{false};
    std::vector<std::jthread> workers_;
};
}
