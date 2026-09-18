// =============================================================================
// 文件名：Connection.h
// 所属模块：server/transport —— 单条 TCP 连接的"档案袋"聚合体
//
// 【职责比喻：一位客人的"客房档案袋"】
//   每来一位客人（新 fd），楼层经理（SubReactor）就为他建一个档案袋（Connection）。
//   档案袋里分三层夹页：传输层（ConnTransport，记 fd/读写缓冲/出站队列/票据）、
//   定时器层（ConnTimer，记超时槽位/心跳/Ping-Pong）、会话层（shared_ptr<Session>，
//   多协议接线员岗位牌）。再附一张协程排班表（CoroutineSlots，记读/写/执行三路协程句柄）。
//   档案袋用"引用别名"把内层字段直接暴露成 conn.fd / conn.id，外层无需穿透写法，读写更顺手。
//   2.0 新增 OutboundQueue 作为出站入口，任务投递进 ConnTransport.outboundQueue；
//   真正写 socket 由 TransportWriter 在 writerLoop 中统一完成，Session 不直接 send。
//
// 关键技术点（初学者重点理解）：
//   1. 【分层聚合】ConnTransport（I/O 状态）+ ConnTimer（定时器）+ Session（协议会话）
//      + CoroutineSlots（协程槽）四件打包。分层让职责清晰：超时逻辑只摸 timer，
//      I/O 逻辑只摸 transport，协议逻辑只摸 session，互不串扰。
//   2. 【引用别名暴露内层】int &fd = transport.fd; 这类别名让外层直接写 conn.fd，
//      等价于穿透访问 transport.fd，但写法更简洁。这是聚合体常用的"扁平化"技巧。
//   3. 【多协议会话基类指针】shared_ptr<Session> 让同一份 Connection 结构可同时承载
//      HttpSession 与 WebSocketSession，升级时直接 reset 替换指针即可交接，是支持
//      多协议的关键。Session 基类不抽象编解码器与发送器，出站统一走 OutboundTask 体系。
//   4. 【票据序号机制】nextTicket / completedTicket / waitingTicket 三连：出站任务领号
//      （nextTicket++）、完成回填（completedTicket）、协程等待特定号（waitingTicket），
//      让 HTTP 协程能 co_await 某条响应真正写完，而不是"入队即返回"。
//   5. 【写背压水位】pendingWriteBytes 配合 kWriteHighWatermark / kWriteLowWatermark：
//      积压超过高水位就暂停读（pauseByWrite），跌回低水位再恢复，防止慢客户端把内存撑爆。
//   6. 【禁拷贝禁移动】Connection 持有 Buffer / deque / shared_ptr 等资源，且被 SubReactor
//      的 conns 表用 unique_ptr 独占，拷贝/移动会破坏唯一归属，故全 delete。
// =============================================================================
#pragma once
#ifndef TRANSPORT_CONNECTION_H
#define TRANSPORT_CONNECTION_H

#include "server/Buffer/Buffer.h"
#include "server/CoroutineScheduler/CoroutineSlot.h"
#include "server/session/Session/Session.h"
#include "server/transport/ConnectionKey.h"
#include "server/transport/OutboundTask.h"
#include "server/tls/TlsTransport.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>

// ---- 出站/读侧的几道"水位线"常量：背压与上限的标尺 ----
inline constexpr std::size_t kMaxPendingReadBytes = 1024UL * 1024UL;   // 单连接读缓冲上限：1MiB，超此认为请求过大
inline constexpr std::size_t kWriteHighWatermark = 4UL * 1024UL * 1024UL; // 写积压高水位：4MiB，超过则暂停读
inline constexpr std::size_t kWriteLowWatermark = 2UL * 1024UL * 1024UL;  // 写积压低水位：2MiB，跌回则恢复读
inline constexpr std::size_t kMaxOutboundTasks = 4096;                    // 单连接出站任务条数上限，防队列无限增长

/**
 * @brief 传输层状态：fd / 读缓冲 / 出站队列 / 票据 / 写背压
 *
 * 【通俗解释】档案袋里"传输层"那一夹页：记着客人的房号（fd）、收件箱（readBuffer）、
 *   待发货清单（outboundQueue）、发货单流水号（nextTicket/completedTicket/waitingTicket）、
 *   以及"是否因写积压暂停收件"的开关（pauseByWrite）。出站队列由 OutboundQueue 入口投递、
 *   TransportWriter 消费，本结构只做承载。
 */
struct ConnTransport
{
    int fd = -1;                          // socket 文件描述符；-1 表示未占用
    uint64_t id = 0;                      // 连接代际号，fd 会被内核复用，id 用于拒绝旧连接的迟到通知
    Buffer readBuffer;                    // 读缓冲：跨多次 recv 拼半包
    ConnState state;                      // 连接 I/O 状态机（closed/readPaused/wantWrite 等）
    std::size_t pendingBytes = 0;         // 已读入但尚未被协议层消费的字节数

    std::deque<OutboundTask> outboundQueue; // 出站任务队列：FIFO，由 TransportWriter 逐条 flush
    std::size_t pendingWriteBytes = 0;      // 队列中已登记待写的字节数（背压计数）
    uint64_t nextTicket = 1;                // 下一个出站任务将领取的票据号（单调递增，0 保留给"无票"）
    uint64_t completedTicket = 0;           // 最近一次完成回填的票据号，驱动等待该号的协程恢复
    uint64_t waitingTicket = 0;             // 协程当前正在等待完成的票据号；0 表示无人在等
    bool pauseByWrite = false;              // 写积压触发的读暂停标志；true 时撤销 EPOLLIN 防止雪崩
};

/**
 * @brief 定时器层状态：超时槽位 / WebSocket 心跳 / Ping-Pong
 *
 * 【通俗解释】档案袋里"定时器"那一夹页：记着客人几点该被请走（expireSlot）、是否在时间轮上
 *   （inWheel）、是否是 WebSocket 长连需要心跳（wsHeartbeat）、是否正在等 Pong（waitingPong）。
 *   与传输层隔离，超时逻辑只读改本结构。
 */
struct ConnTimer
{
    uint64_t expireSlot = 0;     // 在时间轮上所在的槽位号
    bool inWheel = false;        // 是否已加入时间轮（避免重复加/漏删）
    bool wsHeartbeat = false;    // 是否启用 WebSocket 心跳模式
    bool waitingPong = false;    // 是否已发 Ping 在等 Pong；超时未收到则认为连接已死
    uint64_t lastActiveSec = 0;  // 最近一次活动时间戳（秒），用于刷新超时
    uint64_t pingTimestampSec = 0; // 最近一次发 Ping 的时间戳，用于 Pong 超时判定
};

/**
 * @brief 单条 TCP 连接的聚合体：传输层 + 定时器 + 会话 + 协程槽
 *
 * 【档案袋 通俗解释】
 *   一位客人一个档案袋：传输层夹页（transport）管 I/O，定时器夹页（timer）管超时，
 *   会话岗位牌（session）管协议，协程排班表（coroutineSlots）管三路协程句柄。
 *   下面那一串引用别名（fd/id/readBuffer/...）把内层字段"扁平化"暴露，外层直接 conn.fd 即可。
 *
 * 【为何用 shared_ptr<Session> 通俗解释】
 *   协程挂起期间连接可能被关、会话可能升级（HTTP→WebSocket）。shared_ptr 让会话对象
 *   的生命周期独立于连接表项：即便 Connection 被销毁，协程手里还持有 Session 引用不至于
 *   野指针；升级时直接 reset 换成 WebSocketSession，旧 HTTP 协程 co_return 退出即可。
 *
 * @note 设计意图：把一条连接的所有状态聚成一坨，分层清晰 + 引用别名扁平访问 + 多协议会话
 *       基类指针。出站发送不在本层做，统一交给 OutboundQueue + TransportWriter + writerLoop。
 */
struct Connection
{
    ConnTransport transport;                // 传输层状态
    ConnTimer timer;                        // 定时器状态
    std::shared_ptr<Session> session;       // 多协议会话基类指针：HttpSession 或 WebSocketSession
    std::unique_ptr<TlsTransport> tls;      // 可选 TLS 包装；明文连接保持空指针
    CoroutineSlots coroutineSlots{};        // 读/写/执行三路协程句柄槽

    // ---- 引用别名：把内层字段扁平化暴露，外层 conn.fd 等价于 conn.transport.fd ----
    int &fd = transport.fd;
    uint64_t &id = transport.id;
    Buffer &readBuffer = transport.readBuffer;
    ConnState &state = transport.state;
    std::size_t &pendingBytes = transport.pendingBytes;
    uint64_t &expireSlot = timer.expireSlot;
    bool &inWheel = timer.inWheel;

    // ---- 协程槽访问器：按角色（读/写/执行）取对应句柄槽，越界由 roleIndex 防护 ----
    CoroutineSlot &slot(CoroutineRole role)
    {
        return coroutineSlots[roleIndex(role)];
    }

    const CoroutineSlot &slot(CoroutineRole role) const
    {
        return coroutineSlots[roleIndex(role)];
    }

    ConnectionKey key() const noexcept
    {
        return ConnectionKey{transport.fd, transport.id};
    }

    Connection() = default;
    // ---- 禁拷贝禁移动：资源唯一归属，归 SubReactor::conns 的 unique_ptr 所有 ----
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    Connection(Connection &&) = delete;
    Connection &operator=(Connection &&) = delete;
};

#endif
