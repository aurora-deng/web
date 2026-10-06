#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace webserver::phase11
{

/**
 * 有界阻塞任务执行器。
 *
 * Reactor 像前台接待，只负责快速收发网络事件；数据库查询像去仓库找货，可能等待磁盘
 * 或网络。二者分开后，慢 SQL 不会把同一个 Reactor 上的其他连接一起卡住。队列上限
 * 则像仓库门口的限流牌：积压达到 capacity 后 submit 返回 false，由协议适配层返回
 * 503/稍后重试，避免任务无限堆积造成 OOM。
 *
 * 任务中的异常会被 Worker 截住，防止异常越过线程入口触发 std::terminate。需要把结果
 * 或错误送回 Reactor 的任务，应在自己的 lambda 内捕获并投递完成事件。
 */
class BoundedExecutor final
{
public:
    using Task = std::function<void()>;

    BoundedExecutor(std::size_t workerCount, std::size_t capacity);
    ~BoundedExecutor();

    BoundedExecutor(const BoundedExecutor &) = delete;
    BoundedExecutor &operator=(const BoundedExecutor &) = delete;

    [[nodiscard]] bool submit(Task task);

    /** 停止接收新任务，执行完已接收任务，然后等待全部 Worker 退出。可重复调用。 */
    void shutdown() noexcept;

    [[nodiscard]] std::size_t queued() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool accepting() const noexcept;

private:
    void workerLoop() noexcept;

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::mutex shutdownMutex_;
    std::condition_variable ready_;
    std::deque<Task> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

/**
 * 类型别名表达用途：Phase 11 的 PostgreSQL Repository 调用只投递到该执行器，
 * 不与 HTTP handler、WebSocket handler 或模型推理共用队列。
 */
using DatabaseExecutor = BoundedExecutor;

} // namespace webserver::phase11
