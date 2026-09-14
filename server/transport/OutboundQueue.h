// =============================================================================
// 文件名：OutboundQueue.h
// 所属模块：server/transport —— 出站任务的"投递窗口"（2.0 新增）
//
// 【职责比喻：发货部的"投递窗口 + 跨班次传话筒"】
//   Session 想发货不直接找发货员（TransportWriter），而是把运单递到投递窗口（OutboundQueue）。
//   本线程的货（enqueue）当场转交 Writer 入队；别的线程（Worker 线程算完业务要发响应）的货
//   走 post：先锁进 pending 待办箱，再按一下叫号铃（eventfd）通知本 Reactor 线程来取。
//   本类不 include SubReactor.h，而是通过两个回调（WakeCallback / UpdateCallback）反向通知
//   Reactor："这个 fd 有货要发，请更新 epoll 事件并唤醒 writerLoop"。这样出站投递逻辑与
//   Reactor 解耦，OutboundQueue 可被 SubReactor 持有而不产生头文件循环依赖。
//
// 关键技术点（初学者重点理解）：
//   1. 【回调解耦】本类不 include SubReactor.h，靠 WakeCallback（唤醒 writerLoop 协程）与
//      UpdateCallback（更新 epoll 事件，如置 wantWrite / rearm）两个 std::function 回调反向
//      通知 Reactor。这是 2.0 的关键设计：出站队列逻辑独立，Reactor 通过构造时注入回调接线。
//   2. 【跨线程投递 post】别的线程要发货时调 post：锁住 pending 队列压任务，再用 eventfd 的
//      "合并写"（notified.exchange）只按一次铃，避免高并发下每个任务都触发一次系统调用。
//   3. 【本线程入队 enqueue】本线程发货走 enqueue：直接调 Writer_.enqueue 入队，成功后回调
//      update_ / wake_ 让 Reactor 更新事件并唤醒 writerLoop 去排空。
//   4. 【票据预约 reserveTicket】协程在构造 OutboundTask 前先 reserveTicket 领号，保证 ticket
//      单调递增；之后 co_await 等该号完成。连接已关则返回 0（无效号）。
//   5. 【processPending 代际校验】Reactor 线程被 eventfd 唤醒后调 processPending：swap 出
//      pending 全部任务，逐个按 fd 查 conns，并用 connId 校验"同 fd 不同连接"的迟到任务直接丢，
//      防止把货发到复用了旧 fd 的新连接上。
//   6. 【唯一真实写入口仍是 TransportWriter】OutboundQueue 只做投递与回调，真正 writev/sendfile
//      还在 Writer 里。本类是 Writer 之上的一层"投递 + 跨线程汇聚"门面。
// =============================================================================
#pragma once
#ifndef OUTBOUND_QUEUE_H
#define OUTBOUND_QUEUE_H

#include "server/transport/OutboundTask.h"
#include "server/transport/OutboundAdmission.h"
#include "server/transport/TransportWriter.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>

struct Connection;

/**
 * @brief 跨线程投递的出站任务（携带 fd + 代际号校验）
 *
 * 【通俗解释】别的线程往 pending 箱子里塞的运单：除了任务本身，还带 fd 和 connId。
 *   Reactor 线程取出来后用 connId 校验"这个 fd 还是不是当初那个连接"，防止 fd 复用导致
 *   把货发错人。
 */
struct PendingOutbound
{
    int fd = -1;            // 目标连接的 fd
    uint64_t connId = 0;    // 投递时的连接代际号；fd 复用后用于拒绝迟到任务
    OutboundTask task;      // 待投递的出站任务
};

/** Reactor 将邮箱任务转入连接队列后的本批统计。 */
struct PendingDrainStats
{
    std::size_t enqueued = 0;
    std::size_t backpressured = 0;
    std::size_t stale = 0;
};

/**
 * @brief 出站任务投递窗口：本线程入队 + 跨线程汇聚 + 回调通知 Reactor
 *
 * 【投递窗口 通俗解释】
 *   两本账：一本 pending（跨线程塞进来的待办，受 mtx_ 保护）+ 一本在 Connection 里的
 *   outboundQueue（Writer 真正消费的队列）。本线程的货直接进 Connection 的队列；别的线程
 *   的货先存 pending，等 Reactor 线程被 eventfd 叫醒后 processPending 转移过去。
 *   成功入队后用 wake_ / update_ 两个回调通知 Reactor："有货了，快唤醒 writerLoop 发货"。
 *
 * 【为何不 include SubReactor.h 通俗解释】
 *   若 OutboundQueue 直接摸 SubReactor，会形成头文件循环依赖（SubReactor 持有 OutboundQueue
 *   成员，OutboundQueue 又 include SubReactor）。改用回调注入：构造时把 Reactor 的"唤醒 /
 *   更新事件"两个函数传进来，OutboundQueue 只认 std::function 接口，不认 Reactor 类型，
 *   依赖方向单向干净。
 *
 * @note 2.0 新增：SubReactor 持有 OutboundQueue 成员（配合 sessionFactory_），不再直接依赖
 *       wsManager_/wsDispatcher_ 做出站；出站统一走本类 + TransportWriter + writerLoop。
 */
class OutboundQueue
{
public:
    using WakeCallback = std::function<void(int fd)>;    // 唤醒回调：通知 Reactor 唤醒该 fd 的 writerLoop 协程
    using UpdateCallback = std::function<void(int fd)>;  // 更新回调：通知 Reactor 更新该 fd 的 epoll 事件（如置 wantWrite / rearm）

    /**
     * @brief 构造：注入 Writer 引用 + 两个回调
     * @param writer 真实写入口（TransportWriter）
     * @param wake 唤醒 writerLoop 的回调（Reactor 注入）
     * @param update 更新 epoll 事件的回调（Reactor 注入）
     */
    OutboundQueue(TransportWriter &writer,
                  WakeCallback wake,
                  UpdateCallback update,
                  OutboundPostLimits limits = {});
    ~OutboundQueue();

    /**
     * @brief 本线程入队：直接转交 Writer，成功后回调通知 Reactor
     * @return Ok/Backpressure/Closed，同 Writer.enqueue 的语义
     *
     * 【通俗解释】本线程发货：把运单交给 Writer 压进 Connection 的出站队列，然后按一下
     *   update_ 和 wake_ 两个回调，让 Reactor 知道"这个 fd 有货要发"。
     */
    EnqueueResult enqueue(Connection &conn, OutboundTask task);

    /**
     * @brief 预约下一张出站票据号（单调递增）
     * @return 票据号；连接已关返回 0（无效号）
     *
     * 【通俗解释】协程发货前先领号：nextTicket++ 拿一个流水号，之后构造 OutboundTask 带上它，
     *   写完后 Writer 回填 completedTicket，协程就能 co_await 等到"我这单发完了"。
     */
    uint64_t reserveTicket(Connection &conn);

    /**
     * @brief 跨线程投递：通过全局/单连接准入后，锁进 pending 箱并用 eventfd 按铃
     * @param eventFd Reactor 的 eventfd，按铃用
     * @param notified Reactor 的"已通知"标志，做合并写防重复按铃
     * @return Ok 表示邮箱接单；Backpressure 表示全局或目标连接配额已满；Invalid 表示 key 非法
     *
     * 【通俗解释】别的线程发货先过邮箱门卫，再把运单锁进 pending 箱并按 eventfd 叫号铃。
     *   notified 保证连续多次 post 只按一次铃，Reactor 醒来后一次性取走本批任务。
     */
    EnqueueResult post(int fd,
                       uint64_t connId,
                       OutboundTask task,
                       int eventFd,
                       std::atomic<bool> &notified);

    /**
     * @brief Reactor 线程消费 pending：swap 出全部，归还邮箱配额，再校验并转入连接队列
     * @param conns Reactor 的连接表，按 fd 查 Connection
     * @return 本批成功、因连接背压丢弃、以及代际失效的数量
     *
     * 【通俗解释】Reactor 被 eventfd 叫醒后调这个：把 pending 箱整个倒出来，逐张运单按 fd 查
     *   连接表；fd/connId 失效直接丢，连接队列满则按“丢最新”策略拒绝。
     */
    PendingDrainStats processPending(
        std::unordered_map<int, std::unique_ptr<Connection>> &conns);

    /** 只用于监控和测试；返回调用瞬间的邮箱任务数/待发送字节数。 */
    std::size_t pendingTaskCount() const;
    std::size_t pendingWireBytes() const;

private:
    TransportWriter &writer_;                 // 真实写入口
    WakeCallback wake_;                        // 唤醒 writerLoop 的回调
    UpdateCallback update_;                    // 更新 epoll 事件的回调
    OutboundAdmission admission_;              // Reactor 邮箱和单连接的两级准入策略
    mutable std::mutex mtx_;                   // 保护 pending_ 与统计（跨线程入口）
    std::queue<PendingOutbound> pending_;      // 跨线程待办队列
};

#endif
