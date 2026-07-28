#pragma once
#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <functional>
#include "server/threadpoll/thread_pool.h"
/*
 *业务执行器，缓解reactor主线程压力，
 *核心问题：当前 codec.dispatch(ctx) 在协程中同步调用 router.handle，
 * handler 里的 sleep/阻塞IO 会卡住整个 Reactor 线程。
 *
 * 解决方案：Executor 封装 ThreadPool，handler 提交到 Worker 线程执行，
 * 完成后通过完成队列通知 Reactor 续发响应。
 * 主要作用类似addTask（被携程替换了）,用于缓解接受和处理之间在同一位置造成的业务处理卡住导致业务接受卡住
 */
class Executor
{
public:
    using Task = std::function<void()>;

    /**
     * @brief 构造函数
     * @param workerCount Worker 线程数量
     */
    explicit Executor(size_t workerCount = 4) : pool(workerCount) {}

    /**
     * @brief 提交任务到 Worker 线程
     * @param task 要执行的业务函数
     * @return true=提交成功，false=队列满（背压）
     */
    bool submit(Task task)
    {
        return pool.addTask(std::move(task));
    }

private:
    ThreadPool pool;
};


#endif
