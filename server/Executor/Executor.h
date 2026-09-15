// =============================================================================
// 文件名：Executor.h
// 职责：定义业务执行器 Executor——封装线程池，把耗时的 handler 业务逻辑
//       从 Reactor I/O 线程搬到 Worker 线程异步执行，避免阻塞事件循环。
//
// 【生活比喻】
// Reactor 线程是"前厅经理"，专门负责接客、传话，绝不能在后厨炒菜（执行业务），
// 否则整家酒店的客人都被晾着。Executor 就是"后厨经理"，前厅把订单（handler）
// 丢给后厨经理，后厨经理分派给一群厨师（Worker 线程）炒菜；菜炒好后厨师按铃
// （completeQueue + eventfd）通知前厅来端菜。前厅经理始终只做"轻活"，重活全在后厨。
//
// 关键技术点（初学者重点理解）：
// 1. 为什么要 Executor：原本 codec.dispatch(ctx) 在协程里同步调 router.handle，
//    如果 handler 里有 sleep/阻塞 I/O，整个 Reactor 线程就被卡死——所有连接都僵住。
//    Executor 把 handler 投递到线程池异步跑，Reactor 线程立即可继续服务其他连接。
// 2. submit 返回 bool：队列满时返回 false（背压），调用方（HttpSession）据此
//    直接返回 503 Service Unavailable，而不是无限堆积任务把内存撑爆。
// 3. Executor 是 ThreadPool 的薄封装：只暴露 submit 接口，隐藏线程池细节。
//    未来若要换成"协程调度器内执行"或"优先级队列"，只需改 Executor 实现。
// 4. Executor 由 ServerRuntime 持有：所有 SubReactor 共用 HTTP Executor，所有 WS
//    Session 共用另一条 WS Executor。两池隔离拥塞，同时避免每个 Reactor 各开一组线程。
// 5. 每次工作单带 steady-clock deadline；它和 stop_token 都是协作信号，不会强杀线程。
// =============================================================================
#pragma once
#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <functional>
#include <chrono>
#include "server/thread_pool/thread_pool.h"
/*
 *业务执行器，缓解reactor主线程压力，
 *核心问题：当前 codec.dispatch(ctx) 在协程中同步调用 router.handle，
 * handler 里的 sleep/阻塞IO 会卡住整个 Reactor 线程。
 *
 * 解决方案：Executor 封装 ThreadPool，handler 提交到 Worker 线程执行，
 * 完成后通过完成队列通知 Reactor 续发响应。
 * 主要作用类似addTask（被携程替换了）,用于缓解接受和处理之间在同一位置造成的业务处理卡住导致业务接受卡住
 */

/**
 * @brief 业务执行器：把 handler 投递到线程池异步执行
 *
 * 【通俗解释】
 * 后厨经理（Executor）管着一群厨师（ThreadPool）。前厅丢来一个订单（task），
 * 后厨经理把它塞进任务队列，某个空闲厨师领走去做。如果订单队列满了（背压），
 * 后厨经理直接拒单（submit 返回 false），让前厅返回 503。
 */
class Executor
{
public:
    // Task 类型别名：一个无参可调用对象（lambda/function/functor 都行）
    using Task = std::function<void()>;

    /**
     * @brief 构造函数
     * @param workerCount Worker 线程数量（默认 4；Runtime 把 4~32 的总数近似均分到两池）
     *
     * 【通俗解释】招多少个厨师。一般等于 CPU 核数，多了反而线程切换开销大。
     */
    explicit Executor(
        size_t workerCount = 4,
        std::chrono::milliseconds handlerTimeout = std::chrono::seconds{5})
        : pool(workerCount),
          handlerTimeout_(handlerTimeout.count() > 0
                              ? handlerTimeout
                              : std::chrono::seconds{5})
    {
    }

    /**
     * @brief 提交任务到 Worker 线程
     * @param task 要执行的业务函数
     * @return true=提交成功，false=队列满（背压）
     *
     * 【通俗解释】
     * 把订单塞进后厨任务队列，并通知一个空闲厨师来领。如果队列已满（默认 4096），
     * 直接返回 false 让前厅走 503 流程，避免无限堆积任务撑爆内存。
     * 这是"背压"机制的关键出口——上层据此感知过载并降级。
     */
    bool submit(Task task)
    {
        return pool.addTask(std::move(task));
    }

    /** 停止接收新业务，排空已提交业务并等待 Worker 退出；可重复调用。 */
    void shutdown() noexcept
    {
        pool.shutdown();
    }

    /** 为新业务生成基于 steady_clock 的截止时间，不受系统时钟校准影响。 */
    std::chrono::steady_clock::time_point handlerDeadline() const noexcept
    {
        return std::chrono::steady_clock::now() + handlerTimeout_;
    }

    std::chrono::milliseconds handlerTimeout() const noexcept
    {
        return handlerTimeout_;
    }

private:
    // 内部持有的线程池实例——Executor 只是它的薄封装
    ThreadPool pool;
    std::chrono::milliseconds handlerTimeout_;
};


#endif
