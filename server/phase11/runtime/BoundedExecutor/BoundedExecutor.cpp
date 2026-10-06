#include "server/phase11/runtime/BoundedExecutor/BoundedExecutor.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace webserver::phase11
{

BoundedExecutor::BoundedExecutor(std::size_t workerCount,
                                 std::size_t capacity)
    : capacity_(capacity)
{
    if (workerCount == 0)
        throw std::invalid_argument("worker count must be positive");
    if (capacity_ == 0)
        throw std::invalid_argument("executor capacity must be positive");

    workers_.reserve(workerCount);
    try
    {
        for (std::size_t index = 0; index < workerCount; ++index)
            workers_.emplace_back([this] { workerLoop(); });
    }
    catch (...)
    {
        // 若创建第 N 个线程失败，先安全回收已经启动的线程再把异常交给调用方。
        shutdown();
        throw;
    }
}

BoundedExecutor::~BoundedExecutor()
{
    shutdown();
}

bool BoundedExecutor::submit(Task task)
{
    if (!task)
    {
        std::lock_guard lock(mutex_);
        ++rejected_;
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || tasks_.size() >= capacity_)
        {
            ++rejected_;
            return false;
        }
        tasks_.push_back(std::move(task));
        ++submitted_;
        queueHighWatermark_ = std::max(queueHighWatermark_, tasks_.size());
    }
    ready_.notify_one();
    return true;
}

void BoundedExecutor::shutdown() noexcept
{
    std::lock_guard shutdownLock(shutdownMutex_);
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();

    // shutdown 也可能由析构再次调用；joinable 检查使操作保持幂等。
    for (auto &worker : workers_)
    {
        if (worker.joinable())
            worker.join();
    }
}

std::size_t BoundedExecutor::queued() const noexcept
{
    std::lock_guard lock(mutex_);
    return tasks_.size();
}

bool BoundedExecutor::accepting() const noexcept
{
    std::lock_guard lock(mutex_);
    return !stopping_;
}

BoundedExecutor::Snapshot BoundedExecutor::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    return {submitted_, rejected_, completed_, failed_, active_, tasks_.size(),
            queueHighWatermark_, workers_.size(), capacity_, !stopping_};
}

void BoundedExecutor::workerLoop() noexcept
{
    for (;;)
    {
        Task task;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty())
                return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
            ++active_;
        }

        bool failed = false;
        try
        {
            task();
        }
        catch (...)
        {
            failed = true;
            // 线程入口绝不能放出异常。任务需要反馈错误时应自行投递完成事件。
        }
        {
            std::lock_guard lock(mutex_);
            --active_;
            ++completed_;
            if (failed)
                ++failed_;
        }
    }
}

} // namespace webserver::phase11
