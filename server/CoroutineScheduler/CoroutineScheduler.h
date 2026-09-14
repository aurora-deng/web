// =============================================================================
// 文件名：CoroutineScheduler.h
// 职责：声明协程调度器 CoroutineScheduler——管理协程句柄的就绪队列、
//       生命周期所有权、销毁回收，是 SubReactor 内部的"协程总管"。
//
// 【生活比喻】
// 把 SubReactor 想象成一家大酒店的前厅，协程是服务员。前厅经理（调度器）
// 手里有一块"待召回服务员"白板（readyQueue）和一本"在岗服务员花名册"（owned）。
// 服务员挂起后，等事件来了，经理把他的工号写到白板上；轮到出勤时按白板顺序
// 一个个叫醒服务员继续工作。服务员干完活（co_return）后经理把工号从花名册
// 划掉、销毁其更衣柜（destroy 协程帧）。所有"召回/销毁"都必须经过经理，
// 不能让 epoll 回调直接 resume/destroy，否则容易出现重入或悬空句柄。
//
// 关键技术点（初学者重点理解）：
// 1. 协程句柄所有权：Task 通过 release() 把协程帧所有权交给调度器（adopt），
//    调度器在协程 done() 后唯一路径 reap() 中 destroy。这样保证不会 double-free。
// 2. scheduled 集合做去重：同一个句柄在已被加入就绪队列但还没执行前，再次
//    schedule 会直接跳过，避免一个协程被重复入队导致重复 resume。
// 3. owner（shared_ptr<void>）持有 session 共享所有权：保证协程帧销毁前
//    HttpSession/WebSocketSession 对象仍然存活，避免协程恢复时访问已析构的 session。
// 4. CompletionCallback：协程完成时的回调钩子，SubReactor 借此清理 conns 表里
//    对应的 handle 引用，让"完成"这件事可被外部观测。
// =============================================================================
#pragma once
#ifndef COROUTINE_SCHEDUER_H
#define COROUTINE_SCHEDUER_H
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include<functional>
#include<memory>
#include <queue>
#include <utility>
#include <unordered_map>
#include<unordered_set>
#include "server/CoroutineScheduler/CoroutineSlot.h"

// =============================================================================
// CoroutineScheduler：协程调度器（"协程总管 / 前厅经理"）
// =============================================================================
// 用于保存handle
class CoroutineScheduler
{
public:
    // std::coroutine_handle<> 是 C++20 协程的"句柄"，相当于一个可以 resume/destroy 的指针
    using Handle = std::coroutine_handle<>;
    // 完成回调：协程 co_return 后调用，参数为 (fd, handle)，供 SubReactor 做后续清理
    using CompletionCallback =
        std::function<void(int, uint64_t, CoroutineRole, Handle)>;

    ~CoroutineScheduler();

    /**
     * @brief 接收一个外部创建好的协程帧所有权并立即入队
     * @param fd 协程关联的 socket fd（用于完成回调里定位连接）
     * @param h 协程句柄
     * @param owner 持有 session 的 shared_ptr，保证协程存活期间 session 不被析构
     *
     * 【通俗解释】
     * 新来的服务员入职：把他的工号记进花名册（owned），同时让他立即排进待召回队列
     * （schedule）。owner 像是"员工宿舍床位"——只要协程还在岗，session 对象就保证
     * 不被销毁，避免协程恢复时访问已析构成员。
     */
    // 接收 Task::release() 转移过来的协程帧所有权；owner 保证成员协程的
    // HttpSession 在协程帧销毁前仍然存活。
    void adopt(int fd,
               uint64_t connId,
               CoroutineRole role,
               Handle h,
               std::shared_ptr<void> owner = {});

    /**
     * @brief 将已由调度器持有的协程加入就绪队列
     * @param h 协程句柄
     *
     * 【通俗解释】把服务员工号写到"待召回"白板上，等 runReady 时按顺序叫醒。
     *            scheduled 集合保证同一协程不会被重复写两次。
     */
    // 将已由调度器持有的协程加入就绪队列。
    void schedule(Handle h);

    /**
     * @brief 设置协程完成回调（SubReactor 用它来清理 conns 表里的句柄引用）
     * @param callback 回调函数对象
     */
    void setCompletionCallback(CompletionCallback callback);
    std::size_t ownedCount() const { return owned.size(); }

    /**
     * @brief 一次性把就绪队列里所有协程挨个 resume，直到队列空
     *
     * 【通俗解释】前厅经理开始按白板顺序叫号：叫到一个就让他去工作，
     *            工作中如果他又把自己挂起（co_await 别的事件），则继续叫下一个；
     *            如果他干完了（done()），就回收他的工号并销毁更衣柜（reap）。
     */
    void runReady();

private:
    /**
     * @brief 调度器内部记录"我拥有的协程"的结构
     * handle - 协程句柄
     * fd     - 关联的 socket（完成回调用）
     * owner  - session 共享指针，保证协程存活期间 session 不析构
     */
    struct OwnedCoroutine
    {
        Handle handle;
        int fd;
        uint64_t connId;
        CoroutineRole role;
        std::shared_ptr<void> owner;
    };

    /**
     * @brief 回收一个已完成（done）的协程：触发回调、销毁协程帧
     * @param h 已 done 的协程句柄
     *
     * 【通俗解释】服务员干完活了：通知 HR（completionCallback）登记离职、
     *            从花名册划掉工号、销毁更衣柜（handle.destroy()）。
     *            这是协程帧销毁的唯一入口，避免散落各处的 destroy 调用。
     */
    void reap(Handle h);

    // ----- 调度器内部数据结构 -----
    std::queue<Handle> readyQueue;                       // 待召回协程队列（FIFO 白板）
    std::unordered_map<void*,OwnedCoroutine> owned;      // 已拥有的协程花名册（key=handle 地址）
    std::unordered_set<void*> scheduled;                 // 已入队但还没 resume 的去重集合
    CompletionCallback completionCallback;               // 协程完成回调钩子
};

#endif
