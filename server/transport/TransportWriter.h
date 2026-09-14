// =============================================================================
// 文件名：TransportWriter.h
// 所属模块：server/transport —— 唯一的 socket 写入口
//
// 【职责比喻：发货部的"发货员"】
//   出站队列（ConnTransport.outboundQueue）里堆着一张张 OutboundTask 运单，TransportWriter
//   就是真正把货送上车（写 socket）的发货员。它只认两种货：编好的字节串（encoded）用 writev
//   聚集写、Http 响应（http）用 writev + sendfile 混合写。写不动了（EAGAIN）就回来报告
//   Blocked，让 writerLoop 去 rearm EPOLLOUT 等下次可写；写完了就 completeFront 弹单、
//   回填票据。所有写 socket 的系统调用都收口在这里，Session / OutboundQueue 都不直接 send。
//
// 关键技术点（初学者重点理解）：
//   1. 【唯一写入口】TransportWriter 是全系统唯一调用 writev / sendfile 的地方。把写路径
//      收口，便于统一处理 EAGAIN / EINTR / 错误，也避免多处并发写同一 fd 互相踩。
//   2. 【入队即背压判定】enqueue() 在压队前检查积压字节数与队列条数，超 kWriteHighWatermark
//      就返回 Backpressure 并置 pauseByWrite，让上层暂停读，防止慢客户端把内存撑爆。
//   3. 【公平写预算】flush() 默认每轮最多实际写 256KiB；队列未空时返回 Yielded，重新
//      经过 epoll 调度，防止一个大连接长期占用 Reactor。
//   4. 【writev 聚集写】flushEncoded() 把队列前若干张字节串单的剩余段拼进 iovec[64] 一次
//      writev，最多 64 段 / 64KiB，省系统调用次数，是出站热路径的关键优化。
//   5. 【sendfile 零拷贝】flushHttp() 对文件型 body 走 sendfile，绕过用户态拷贝，静态文件
//      高性能；非文件走 writeMemoryBody 分段 writev。
//   6. 【水位恢复】写积压跌回 kWriteLowWatermark 时撤销 pauseByWrite、恢复读，形成完整的
//      写背压闭环：高水位暂停读 → 消化 → 低水位恢复读。
//   7. 【ticket 回填】completeFront() 把完成单的 ticket 回填进 completedTicket，驱动等待
//      该号的协程恢复（HTTP 协程 co_await 发送完成靠这个）。
// =============================================================================
#pragma once
#ifndef TRANSPORT_WRITER_H
#define TRANSPORT_WRITER_H

#include "server/transport/Connection.h"
#include "server/transport/EnqueueResult.h"

#include <cstddef>

class SegmentPool;
class TimerWheel;

/**
 * @brief 单次 flush 的结果状态
 *
 * 【通俗解释】Drained = 队列已排空；Blocked = 写到 EAGAIN；Yielded = 本轮公平预算用完；
 *   Error = 写出错，连接该关了。
 */
enum class FlushStatus
{
    Drained,
    Blocked,
    Yielded,
    Error
};

// 单个连接一次被 Writer 连续写入的最大字节数；到点后把执行权还给 epoll。
inline constexpr std::size_t kWriteQuantumBytes = 256 * 1024;

/**
 * @brief flush 的完整结果：状态 + 是否请求关连接
 *
 * 【通俗解释】除了状态，还带一个 closeRequested 标志：当某张单的 completion 是
 *   CloseConnection 时，写完它就要求关连接。Writer 不直接关，只把信号带回给调用方。
 */
struct FlushResult
{
    FlushStatus status = FlushStatus::Drained;
    bool closeRequested = false;
    std::size_t bytesWritten = 0;
};

/**
 * @brief socket 写入口：把出站队列里的任务真正写到 fd 上
 *
 * 【发货员 通俗解释】
 *   手里两件工具：writev（聚集写字节串）+ sendfile（零拷贝发文件）。接到 flush 指令就
 *   循环取队首发货，写满 EAGAIN 就歇（Blocked），预算用完主动让班（Yielded），写完就弹单回填票据。背压由 enqueue 在
 *   入队时判定，水位恢复在 flush 排空后检查。
 *
 * @note 持有 SegmentPool（分段对象池，供 writev 拼 iovec）与 TimerWheel（写成功后 refresh
 *       连接活跃度，防超时误杀）的引用。本类无状态（不含连接相关成员），可被多个 SubReactor
 *       共享或各持一份。
 */
class TransportWriter
{
public:
    TransportWriter(SegmentPool &pool, TimerWheel &wheel)
        : pool_(pool), wheel_(wheel)
    {
    }

    EnqueueResult enqueue(Connection &conn, OutboundTask task);  // 入队 + 背压判定
    FlushResult flush(Connection &conn,
                      std::size_t byteBudget = kWriteQuantumBytes);
    void cancelAll(Connection &conn, OutboundOutcome outcome);

private:
    FlushStatus flushEncoded(Connection &conn,
                             bool &closeRequested,
                             std::size_t &byteBudget);
    FlushStatus flushHttp(Connection &conn,
                          bool &closeRequested,
                          std::size_t &byteBudget);
    void completeFront(Connection &conn, bool &closeRequested);         // 弹队首 + 回填 ticket + 检 CloseConnection
    void consumeBufferedBytes(Connection &conn, std::size_t bytes);     // 扣减待写计数 + 低水位恢复读

    SegmentPool &pool_;   // 分段对象池：buildSegments 拼 iovec
    TimerWheel &wheel_;   // 时间轮：写成功后 refresh 防 fd 超时
};

#endif
