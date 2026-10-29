#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
namespace nebula::client {
/** 限制“运行中+排队”的总任务数；满载明确拒绝，不给长连接偷偷扩线程。 */
class BoundedPool final {
public:
    BoundedPool(std::size_t workers,std::size_t capacity);
    ~BoundedPool();
    bool submit(std::function<void()>task);
    void shutdown();
private:
    std::mutex mutex_;std::condition_variable changed_;std::deque<std::function<void()>>queue_;
    std::vector<std::thread>workers_;std::size_t capacity_,pending_{};bool stopping_{};
};
}
