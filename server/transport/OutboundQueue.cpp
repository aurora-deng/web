// =============================================================================
// 文件名：OutboundQueue.cpp
// 所属模块：server/transport —— 出站投递窗口的实现（2.0 新增）
//
// 【职责比喻：投递窗口的"操作台"】
//   本文件实现 OutboundQueue 的四件事：构造时绑定 Writer + 回调；本线程入队 enqueue（转交
//   Writer + 回调通知）；票据预约 reserveTicket（nextTicket 单调自增）；跨线程投递 post
//   （锁 pending + eventfd 合并按铃）；Reactor 线程消费 processPending（swap 出全部 + 代际校验）。
//
// 关键技术点（初学者重点理解）：
//   1. 【eventfd 合并按铃】post 用 notified.exchange(true) 做合并：只有第一个把 false 换成
//      true 的线程才真正 write eventfd，后续 post 只压队列不再按铃。Reactor 被叫醒一次就把
//      pending 全清，高并发下省掉大量 eventfd 写系统调用。
//   2. 【swap 锁内快出】processPending 在锁内只做 local.swap(pending_)，把整队瞬间搬走，
//      锁外再慢慢逐个校验入队，最小化持锁时间。
//   3. 【代际号校验】processPending 按 fd 查 conns，并用 connId 比对 Connection::id：fd 不在
//      表里或 id 不符（fd 已被复用给新连接）的任务直接 continue 丢弃，防发错人。
//   4. 【回调幂等】enqueue 成功后才调 update_ / wake_；回调由 Reactor 注入，负责置 wantWrite /
//      rearm / 唤醒 writerLoop 协程，本类不关心具体实现。
// =============================================================================
#include "OutboundQueue.h"

#include "log/logger/logger.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

// 构造：绑定 Writer + 两个回调（move 进成员，避免多余拷贝）
OutboundQueue::OutboundQueue(TransportWriter &writer,
                             WakeCallback wake,
                             UpdateCallback update,
                             OutboundPostLimits limits)
    : writer_(writer),
      wake_(std::move(wake)),
      update_(std::move(update)),
      admission_(limits)
{
}

OutboundQueue::~OutboundQueue()
{
    std::lock_guard<std::mutex> lock(mtx_);
    while (!pending_.empty())
    {
        pending_.front().task.complete(OutboundOutcome::Closed);
        pending_.pop();
    }
    admission_.reset();
}

// 本线程入队：连接已关直接 Closed；否则转交 Writer，Ok 时回调通知 Reactor
EnqueueResult OutboundQueue::enqueue(Connection &conn, OutboundTask task)
{
    if (conn.state.closed)
    {
        task.complete(OutboundOutcome::Closed);
        return EnqueueResult::Closed;          // 连接已死，丢弃
    }

    const auto result = writer_.enqueue(conn, std::move(task));  // 真正入队由 Writer 做
    if (result == EnqueueResult::Ok)
    {
        if (update_)
            update_(conn.fd);                   // 通知 Reactor 更新 epoll 事件（如置 wantWrite）
        if (wake_)
            wake_(conn.fd);                     // 通知 Reactor 唤醒该 fd 的 writerLoop 协程
    }
    else if (result == EnqueueResult::Backpressure && update_)
    {
        // Writer 已置 pauseByWrite；即使本任务被拒，也必须立即撤销 EPOLLIN。
        update_(conn.fd);
    }
    return result;
}

// 票据预约：连接已关返回 0（无效号）；否则 nextTicket 单调自增后返回原值
uint64_t OutboundQueue::reserveTicket(Connection &conn)
{
    if (conn.state.closed)
        return 0;                               // 0 号保留给"无需等待 / 无效"
    return conn.transport.nextTicket++;         // 后置自增：返回当前号，下一张用 +1 后的号
}

// 跨线程投递：锁进 pending + eventfd 合并按铃
EnqueueResult OutboundQueue::post(int fd,
                                  uint64_t connId,
                                  OutboundTask task,
                                  int eventFd,
                                  std::atomic<bool> &notified)
{
    if (fd < 0 || connId == 0)
    {
        task.complete(OutboundOutcome::Invalid);
        return EnqueueResult::Invalid;
    }

    const ConnectionKey key{fd, connId};
    const std::size_t bytes = task.remainingBytes();
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!admission_.tryReserve(key, bytes))
        {
            task.complete(OutboundOutcome::Backpressured);
            return EnqueueResult::Backpressure;
        }

        pending_.push(PendingOutbound{fd, connId, std::move(task)});
    }
    // ---- 合并按铃：只有第一个把 false 换成 true 的线程才真正 write eventfd ----
    if (!notified.exchange(true))
    {
        uint64_t one = 1;
        if (write(eventFd, &one, sizeof(one)) == -1 && errno != EAGAIN)
        {
            LOG_INFO(std::string("OutboundQueue::post eventfd write error: ") +
                     strerror(errno));  // eventfd 写罕见失败（EAGAIN 也极少），记日志不阻断
        }
    }
    return EnqueueResult::Ok;
}

// Reactor 线程消费 pending：锁内 swap 出全部，锁外逐个校验入队
PendingDrainStats OutboundQueue::processPending(
    std::unordered_map<int, std::unique_ptr<Connection>> &conns)
{
    PendingDrainStats stats;
    std::queue<PendingOutbound> local;
    {
        std::lock_guard<std::mutex> lock(mtx_);  // 锁内只做 swap，瞬间搬走整队
        local.swap(pending_);
        admission_.reset();
    }

    while (!local.empty())
    {
        auto job = std::move(local.front());
        local.pop();

        auto it = conns.find(job.fd);
        // ---- 代际校验：fd 不在表里或 id 不符（fd 已被复用给新连接）直接丢 ----
        if (it == conns.end() || it->second->id != job.connId)
        {
            job.task.complete(OutboundOutcome::Stale);
            ++stats.stale;
            continue;
        }

        const auto result = enqueue(*it->second, std::move(job.task));
        if (result == EnqueueResult::Ok)
            ++stats.enqueued;
        else if (result == EnqueueResult::Backpressure)
            ++stats.backpressured;
        else
            ++stats.stale;
    }
    return stats;
}

std::size_t OutboundQueue::pendingTaskCount() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return admission_.taskCount();
}

std::size_t OutboundQueue::pendingWireBytes() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return admission_.wireBytes();
}
