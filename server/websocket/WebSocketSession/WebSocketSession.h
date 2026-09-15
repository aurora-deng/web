// =============================================================================
// 文件名：WebSocketSession.h
// 所属模块：server/websocket/WebSocketSession —— WebSocket 协议会话生命周期所有者
//
// 【职责比喻：WebSocket 长连接的"长期管家"】
//   HTTP 握手完成后，HttpSession 把 fd 交给 WebSocketSession 接管——此后这条 TCP 连接
//   不再讲 HTTP，而切换成 WebSocket 二进制帧流。WebSocketSession 是这条长连接的"长期
//   管家"：循环读帧 → 解码 → 派发给业务 handler → 把回执组帧发出，并负责心跳 Ping/Pong、
//   分片重组、Close 握手等协议细节。出站发送走协议无关的 OutboundTask + OutboundQueue +
//   TransportWriter + writerLoop 体系，WebSocketSession 不持有 sender。
//
// 【2.0 架构特点（与 1.0 的区别，注释必须如实反映）】
//   - 继承 2.0 的 Session 基类：基类只有 run() 纯虚 + onTimeout/onClose 两个钩子，
//     没有 protocol()。WebSocketSession override run()/onTimeout/onClose 三者。
//   - 出站不抽象 sender：调 enqueueOutbound → SubReactor::enqueueOutbound →
//     OutboundQueue → writerLoop（由 TransportWriter 冲刷到 socket）。Session 层只
//     交付"已编码字节 + 完成回调"，不直接写 fd，也不持有任何发送器成员。
//   - 由 WebSocketSessionFactory 装配：SubReactor 持有 sessionFactory_ 抽象指针，
//     升级时调 createWebSocketSession 把 manager/dispatcher 等依赖注入进 WebSocketSession，
//     SubReactor 自身不直接持有 wsManager_/wsDispatcher_。
//   - shared_from_this：继承 enable_shared_from_this，run() 注册到 SessionManager 时
//     需要 shared_ptr 自引用，避免裸指针被异步销毁。
//
// 关键技术点（初学者重点理解）：
//   1. 【协程主循环】run() 返回 Task<void>，C++20 协程。while(state==Open) 循环：
//      decode → NeedMore 时 co_await ReadAwaiter 挂起等数据 → 收到完整帧后按 opcode 分流。
//   2. 【不持有 sender】出站统一走 enqueueOutbound → SubReactor::outbound_ (OutboundQueue)
//      → writerLoop。Session 只管"把字节丢进队列"，由 writerLoop 协程异步冲刷到 socket，
//      与 HttpSession 共用同一套发送链路。
//   3. 【生命周期钩子】onTimeout 实现心跳：未发过 Ping 就发 Ping+续期，等 Pong；已发过
//      且超时则返回 true 同意关闭。onClose 在 fd_close 前向 manager 注销自己。
//   4. 【分片重组】数据帧交给 WebSocketMessageAssembler；它用显式 active 状态校验
//      分片顺序、限制整条消息大小并完成重组。控制帧仍可穿插在分片之间。
//   5. 【Close 握手】收到对端 Close 帧后回一个 Close（带相同 code/reason），再 co_return
//      关闭连接——RFC 6455 §7.1.4 的关闭握手流程。
//   6. 【状态机 WsSessionState】Open（正常收发）→ Closing（已发 Close 等关闭）→ Closed
//      （协程退出）。onTimeout/finishAppMessage 据此决定是否还允许发数据。
// =============================================================================
#pragma once
#ifndef WEBSOCKET_SESSION_H
#define WEBSOCKET_SESSION_H

#include "server/session/Session/Session.h"
#include "server/transport/EnqueueResult.h"
#include "server/transport/OutboundTask.h"
#include "server/websocket/WebSocketCodec/WebSocketCodec.h"
#include "server/websocket/WebSocketDispatcher/WsMessageContext.h"
#include "server/websocket/WebSocketParser/WebSocketParser.h"
#include "server/websocket/WebSocketMessageAssembler/WebSocketMessageAssembler.h"
#include "server/transport/ConnectionKey.h"
#include "server/websocket/WebSocketSessionManager/WebSocketSessionManager.h"
#include "server/websocket/WebSocketTypes/WebSocketTypes.h"

#include <memory>
#include <stop_token>
#include <string>

// 前置声明：避免头文件循环依赖，实现在 .cpp 中 #include 真正的定义
class SubReactor;
struct Connection;
class WebSocketDispatcher;
class Executor;

/**
 * @brief WebSocket 会话状态机阶段
 *
 * 通俗解释：管家黑板上的"当前状态"标签——Open 正常待客、Closing 已发 Close 等
 *   关闭、Closed 协程将退。run() 主循环和 onTimeout/finishAppMessage 都据此判断
 *   是否还允许继续收发数据。
 */
enum class WsSessionState
{
    Open,     // 正常收发中
    Closing,  // 已发出 Close 帧，等待对端回 Close 或直接关闭
    Closed    // 会话结束，协程即将 co_return
};

/** 提交一条 WS 业务消息后的三种确定结果。 */
enum class WsHandlerStartResult
{
    Submitted,  // 已交给 Worker，根协程必须 co_await ExecuteAwaiter
    Overloaded, // Executor 拒绝，调用方回 Close(1013)
    Closed      // 连接已失效，直接退出
};

/**
 * @brief WebSocket 长连接会话——继承 2.0 Session 基类，循环收发帧并对接业务 Dispatcher
 *
 * 【长期管家 通俗解释】
 *   HTTP 握手成功后，HttpSession 把这条 fd 交给 WebSocketSession 接管。此后管家循环干
 *   三件事：① 用 parser_+codec_ 把字节流拆成帧、翻译成业务消息；② 经 Executor 调
 *   dispatcher 路由到 handler；③ 回到 Reactor 把回执组帧后 enqueueOutbound。心跳、分片、Close
 *   握手都由管家自理，业务 handler 只需填好 WsMessageContext.outbound。
 *
 * 【为何不持有 sender 通俗解释】
 *   2.0 把出站发送抽象成"协议无关的 OutboundTask 流水线"：Session 只要把已编码字节 +
 *   完成回调打包成 OutboundTask 丢进 SubReactor::outbound_ (OutboundQueue)，writerLoop
 *   协程会异步冲刷到 socket（由 TransportWriter 实际写 fd）。这让 HTTP/WebSocket 共用
 *   同一套发送链路，Session 不背 sender 成员，协议层和传输层彻底解耦。
 *
 * @note 继承 enable_shared_from_this：run() 注册到 SessionManager 时需 shared_ptr 自引用，
 *       避免裸指针在异步事件中被销毁造成野引用。
 */
class WebSocketSession : public Session, public std::enable_shared_from_this<WebSocketSession>
{
public:
    /**
     * @brief 构造一个 WebSocketSession
     * @param key HTTP 升级后沿用的连接身份（fd + connId）
     * @param reactor 所属 SubReactor（事件循环 + 调度器 + OutboundQueue）
     * @param uid 用户 id（0 表示匿名，匿名不注册到 manager）
     * @param manager 全局 WebSocketSessionManager 指针（用于注册/注销/跨 Reactor 寻址）
     * @param dispatcher 业务派发器引用（codec_ 内部用它路由消息到 handler）
     * @note 由 WebSocketSessionFactory::createWebSocketSession 调用，外部不直接 new。
     */
    WebSocketSession(ConnectionKey key,
                     SubReactor *reactor,
                     UserId uid,
                     WebSocketSessionManager *manager,
                     WebSocketDispatcher &dispatcher,
                     Executor &executor);

    /** @brief 返回当前会话绑定的用户 id（供 handler/manager 使用） */
    UserId userId() const { return uid_; }

    /**
     * @brief 会话主协程：WebSocket 帧收发主循环（override Session::run）
     * @return Task<void>，C++20 协程
     * @note while(state==Open) 循环：decode → NeedMore 时 co_await ReadAwaiter 挂起等数据；
     *       收到完整帧按 opcode 分流（Close/Ping/Pong/分片/数据帧）。出站走 enqueueOutbound。
     */
    Task<void> run() override;

    /**
     * @brief 超时钩子（override Session::onTimeout）：实现 WebSocket 心跳
     * @return false 表示本轮暂不关闭（已发 Ping，等 Pong 下轮再看）；true 表示同意关闭
     * @note 仅在 conn->timer.wsHeartbeat 开启时执行心跳逻辑；否则直接返回 true。
     */
    bool onTimeout() override;

    /**
     * @brief 连接关闭前清理钩子（override Session::onClose）
     * @note 向 manager 注销自己（unregisterIfNeeded），避免 SessionManager 留下死指针。
     */
    void onClose() override;

    /** 连接关闭或 Runtime 停机时只发撤单信号，不等待 Worker。 */
    void requestHandlerStop() noexcept override;

private:
    /**
     * @brief 通过 ConnectionKey 安全查找当前连接对象
     * @return 存在返回 Connection 指针；连接已销毁返回 nullptr
     * @note 协程挂起期间 fd 可能被关闭并复用，每次恢复都用 fd+connId 重新查表。
     */
    Connection *getConn();

    /** @brief 帧被解析器消费后更新读积压，并在水位恢复时解除内存背压 */
    void afterFrameConsumed(Connection &conn) noexcept;

    /**
     * @brief 把已编码字节封装成 OutboundTask 入队（出站发送的统一入口）
     * @param bytes 已编码的 WS 帧字节
     * @param completion 完成回调标记（None/CloseConnection 等）
     * @return Ok/Backpressure/Closed/Invalid，保留失败原因供上层选择策略
     * @note 不直接写 fd——交给 SubReactor::outbound_ (OutboundQueue) → writerLoop 冲刷。
     */
    EnqueueResult enqueueOutbound(
        std::string bytes,
        OutboundCompletion completion = OutboundCompletion::None);

    /** 业务数据入队失败时执行 WS 策略：过载回 1013，编码错误回 1011。 */
    bool enqueueDataOrClose(std::string bytes);

    /** @brief 若已注册到 manager 则注销（幂等：未注册或 uid==0 时安全无副作用） */
    void unregisterIfNeeded();

    /**
     * @brief 在 Reactor 线程准备工作单，并把业务 handler 提交给 Executor
     * @param opcode 帧类型（Text/Binary）
     * @param payload 已解掩码、已重组的完整 payload
     * @note 此方法不等待任务；只有 run() 这条根协程执行 ExecuteAwaiter。
     */
    WsHandlerStartResult startAppMessage(WsOpcode opcode, std::string payload);

    /** @brief Worker 完成后回到 Reactor 线程，编码并入队 handler 的结果。 */
    bool finishAppMessage();

    SubReactor *reactor_ = nullptr;               // 所属 SubReactor（事件循环+调度器+OutboundQueue），不持有所有权
    ConnectionKey key_{};                         // fd + connId；升级前后保持同一连接身份
    UserId uid_ = 0;                              // 用户 id（0 表示匿名，匿名不注册到 manager）
    WebSocketSessionManager *manager_ = nullptr;  // 全局会话目录指针（不持有所有权），用于注册/注销
    Executor &executor_;                          // WS 专用业务线程池边界
    WebSocketParser parser_;                      // 连接私有帧解析器（跨次 recv 保留状态机进度）
    WebSocketCodec codec_;                        // 编解码+分发（持有 dispatcher 引用）
    WsSessionState state_ = WsSessionState::Open; // 当前会话状态机阶段
    WebSocketMessageAssembler messageAssembler_;  // 数据帧顺序校验、分片重组与整条消息限流
    WsMessageContext messageContext_;             // 一次仅一条：Worker 填写，Reactor 完成阶段消费
    bool handlerDispatched_ = false;               // Dispatcher 是否命中 handler
    bool handlerFailed_ = false;                   // handler 是否抛异常
    bool handlerTimedOut_ = false;                 // Worker 返回时是否已经超过业务截止时间
    std::stop_source handlerStopSource_;           // 当前在途 handler 的协作式撤单源
};

#endif
