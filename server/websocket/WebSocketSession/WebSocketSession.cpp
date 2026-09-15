// ==============================================================================
// 文件名：WebSocketSession.cpp
// 所属模块：server/websocket/WebSocketSession —— WebSocket 长连接会话实现
//
// 【职责比喻：长期管家的"一天工作日志"】
//   接 WebSocketSession.h 的管家比喻：本文件是管家的具体工作流程——
//   怎么构造上岗（构造函数）、怎么查连接表（getConn）、怎么收帧/分流/重组（run）、
//   怎么处理心跳（onTimeout）、怎么清理退场（onClose）、怎么把字节丢进出站队列
//   （enqueueOutbound）、怎么把一条业务消息交给 Worker 并回到 Reactor 收尾。
//
// 【2.0 出站链路（与 1.0 的关键区别）】
//   出站发送不经过 sender，统一走：
//     enqueueOutbound → SubReactor::enqueueOutbound → OutboundQueue::push
//       → writerLoop 协程冲刷 → TransportWriter::write(fd) 实际写 socket
//   Session 层只交付"已编码字节 + 完成回调（OutboundCompletion）"，不直接写 fd，
//   也不持有任何发送器成员。CloseConnection 完成回调让 writerLoop 在发完字节后
//   自动触发 fd_close，实现"发完 Close 帧即关连接"的语义。
//
// 关键技术点（初学者重点理解）：
//   1. 【协程主循环 run()】while(state==Open) 中：先 decode 已有缓冲；NeedMore 时
//      先尝试 recvSocket 非阻塞读，仍不够则 co_await ReadAwaiter 挂起等事件。
//   2. 【连接安全查表】协程挂起期间连接可能被其他线程关闭，每次恢复后 getConn()
//      重新查表，conn 为空或 conn->state.closed 就直接退场，防野指针。
//   3. 【Close 握手】收到对端 Close 帧后回相同 code/reason 的 Close，再 co_return；
//      协议错误时回 Close(ProtocolError)。CloseConnection 回调让 writerLoop 发完即关。
//   4. 【心跳 onTimeout】未发过 Ping 就发 Ping+续期，等下一轮超时再检查 Pong；
//      已发 Ping 且超时未收到 Pong 则返回 true 同意关闭。
//   5. 【分片重组】数据帧交给 WebSocketMessageAssembler；它用显式 active 状态校验顺序、
//      限制整条消息大小并完成重组。控制帧在 Session 中旁路，可穿插在分片之间。
//   6. 【handler 线程隔离】完整消息提交 Executor；根协程在 ExecuteAwaiter 等待，
//      完成后回所属 Reactor 编码和入队。dispatch 未命中时仍保留 echo 兜底。
// ==============================================================================
#include "WebSocketSession.h"

#include "server/CoroutineScheduler/AWaiter.h"
#include "server/Executor/Executor.h"
#include "server/SubReactor/SubReactor.h"
#include "server/websocket/WebSocketDispatcher/WebSocketDispatcher.h"
#include "server/websocket/WebSocketValidation/WebSocketValidation.h"

#include <chrono>

// ---- 构造函数：绑定连接身份 / reactor / uid / manager / dispatcher / executor ----
WebSocketSession::WebSocketSession(ConnectionKey key,
                                   SubReactor *reactor,
                                   UserId uid,
                                   WebSocketSessionManager *manager,
                                   WebSocketDispatcher &dispatcher,
                                   Executor &executor)
    : reactor_(reactor),
      key_(key),
      uid_(uid),
      manager_(manager),
      executor_(executor),
      codec_(dispatcher)
{
}

// ---- 通过 fd+connId 查找当前连接（协程恢复后拒绝已复用 fd） ----
Connection *WebSocketSession::getConn()
{
    return reactor_->findConnection(key_.fd, key_.connId);
}

void WebSocketSession::afterFrameConsumed(Connection &conn) noexcept
{
    conn.pendingBytes = conn.readBuffer.readableBytes();
    if (conn.pendingBytes <= kMaxPendingReadBytes)
        conn.state.pauseByMemory = false;
}

// ---- 注销：若已注册到 manager 则擦除 uid 对应条目（幂等） ----
void WebSocketSession::unregisterIfNeeded()
{
    if (manager_ && uid_ != 0)
        manager_->unregister(uid_, key_);
}

// ---- onClose 钩子：fd_close 前向 manager 注销自己 ----
void WebSocketSession::onClose()
{
    requestHandlerStop();
    unregisterIfNeeded();
}

void WebSocketSession::requestHandlerStop() noexcept
{
    (void)handlerStopSource_.request_stop();
}

/**
 * @brief 超时钩子：实现 WebSocket 心跳 Ping/Pong 探活
 * @return false 本轮暂不关闭（已发 Ping，等 Pong 下轮再看）；true 同意关闭
 *
 * 通俗解释：时间轮到点时 Reactor 问管家"该关了吗"。管家分三种情况：
 *   - 正在 Closing/Closed：返回 false 不让 Reactor 重复关（自己已经在关了）；
 *   - 心跳未启用或连接已没了：返回 true 直接关；
 *   - 还没发过 Ping：发一个 Ping，记下时间戳，续期等待 Pong，返回 false；
 *   - 发过 Ping 但 Pong 超时未回：返回 true 同意关闭（对端可能已僵死）。
 */
bool WebSocketSession::onTimeout()
{
    // 已经在关闭流程中，不让 Reactor 重复触发 fd_close
    if (state_ == WsSessionState::Closing || state_ == WsSessionState::Closed)
        return false;

    auto *conn = getConn();
    if (!conn)
        return true;  // 连接已销毁，同意关闭

    // 心跳未启用（wsHeartbeat=false），直接同意关闭
    if (!conn->timer.wsHeartbeat)
        return true;

    // ---- 还没发过 Ping：发 Ping + 续期，等下一轮再看 Pong ----
    if (!conn->timer.waitingPong)
    {
        // Ping 帧入队失败（如连接已关）则直接同意关闭
        if (enqueueOutbound(WebSocketCodec::encodePing()) != EnqueueResult::Ok)
            return true;
        conn->timer.waitingPong = true;  // 标记正在等 Pong
        // 记录发 Ping 的时间戳，供下一轮判断 Pong 是否超时
        conn->timer.pingTimestampSec = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        reactor_->refreshIdleTimer(key_.fd);  // 续期空闲定时器，给 Pong 留时间
        return false;                     // 本轮不关，等下一轮
    }

    // ---- 发过 Ping 且超时未收到 Pong：对端可能僵死，同意关闭 ----
    return true;
}

/**
 * @brief 把已编码字节封装成 OutboundTask 入队（出站发送的统一入口）
 * @param bytes 已编码的 WS 帧字节
 * @param completion 完成回调标记（None/CloseConnection）
 * @return 保留 Ok/Backpressure/Closed/Invalid，供调用点选择关闭或重试策略
 *
 * 通俗解释：管家不亲自写 socket，只把字节打包成 OutboundTask 丢进 SubReactor::outbound_
 *   (OutboundQueue)，writerLoop 协程会异步冲刷到 socket。CloseConnection 标记让
 *   writerLoop 在发完字节后自动触发 fd_close。
 */
EnqueueResult WebSocketSession::enqueueOutbound(
    std::string bytes,
    OutboundCompletion completion)
{
    if (bytes.empty())
        return EnqueueResult::Invalid;
    auto *conn = getConn();
    if (!conn || conn->state.closed)
        return EnqueueResult::Closed;
    // OutboundTask::encoded 把字节+优先级+完成回调打包；交给 SubReactor 的 OutboundQueue
    return reactor_->enqueueOutbound(
        key_.fd,
        OutboundTask::encoded(std::move(bytes), 0, completion));
}

bool WebSocketSession::enqueueDataOrClose(std::string bytes)
{
    const auto result = enqueueOutbound(std::move(bytes));
    if (result == EnqueueResult::Ok)
        return true;
    if (result == EnqueueResult::Closed)
    {
        state_ = WsSessionState::Closed;
        return false;
    }

    // 业务生产速度超过 socket 发送速度时，停止继续读，并用 1013 明确结束慢连接。
    // Codec 自身拒绝了非法业务输出则属于服务端错误，使用 1011。
    state_ = WsSessionState::Closing;
    const auto code = result == EnqueueResult::Backpressure
        ? WsCloseCode::TryAgainLater
        : WsCloseCode::InternalError;
    (void)enqueueOutbound(
        WebSocketCodec::encodeClose(code),
        OutboundCompletion::CloseConnection);
    return false;
}

/**
 * @brief 准备一条完整应用消息并把 handler 提交给 Executor
 * @param opcode 帧类型（Text/Binary）
 * @param payload 已解掩码、已重组的完整 payload
 *
 * Reactor 像前厅，只负责填单并交给后厨；真正的 handler 在 Worker 执行。Worker 只能
 * 修改 messageContext_，结束后通过 notifyExecuteComplete 叫醒根协程。
 */
WsHandlerStartResult WebSocketSession::startAppMessage(
    WsOpcode opcode, std::string payload)
{
    if (!getConn())
        return WsHandlerStartResult::Closed;

    // ---- 把 payload 装进 WsFrame（fin=true 表示已是完整消息） ----
    WsFrame frame;
    frame.opcode = opcode;
    frame.payload = std::move(payload);
    frame.fin = true;

    // ---- 构造消息上下文，填好 inbound + 会话信息 ----
    messageContext_ = WsMessageContext{};
    messageContext_.uid = uid_;
    messageContext_.manager = manager_;
    messageContext_.inbound = codec_.messageFromFrame(frame);
    messageContext_.inbound.fromUserId = uid_;
    handlerStopSource_ = std::stop_source{};
    messageContext_.cancellation = HandlerCancellation{
        handlerStopSource_.get_token(), executor_.handlerDeadline()};
    handlerDispatched_ = false;
    handlerFailed_ = false;
    handlerTimedOut_ = false;

    const ConnectionKey key = key_;
    auto self = shared_from_this();
    const bool submitted = executor_.submit([self = std::move(self), key]
                                            {
        try
        {
            // 排队期间若连接已关闭或截止时间已到，不再进入业务代码。
            if (!self->messageContext_.stopRequested())
                self->handlerDispatched_ =
                    self->codec_.dispatch(self->messageContext_);
        }
        catch (...)
        {
            self->handlerFailed_ = true;
        }
        self->handlerTimedOut_ =
            self->messageContext_.handlerDeadlineExceeded();

        // 无论成功还是异常都必须通知；连接已关时该通知会唤醒 zombie coroutine 清理。
        self->reactor_->notifyExecuteComplete(key.fd, key.connId); });

    return submitted ? WsHandlerStartResult::Submitted
                     : WsHandlerStartResult::Overloaded;
}

/**
 * @brief 在 Worker 完成后，由所属 Reactor 消费工作单并执行协议/出站操作
 * @return true 可继续读下一条消息；false 已进入关闭流程
 */
bool WebSocketSession::finishAppMessage()
{
    if (handlerTimedOut_)
    {
        messageContext_ = WsMessageContext{};
        state_ = WsSessionState::Closing;
        (void)enqueueOutbound(
            WebSocketCodec::encodeClose(
                WsCloseCode::TryAgainLater, "handler timeout"),
            OutboundCompletion::CloseConnection);
        return false;
    }

    if (handlerFailed_)
    {
        messageContext_ = WsMessageContext{};
        state_ = WsSessionState::Closing;
        (void)enqueueOutbound(
            WebSocketCodec::encodeClose(
                WsCloseCode::InternalError, "handler exception"),
            OutboundCompletion::CloseConnection);
        return false;
    }

    WsMessageContext ctx = std::move(messageContext_);
    messageContext_ = WsMessageContext{};

    // Dispatcher 没命中 handler 时，保留原有的 echo 兜底语义。
    if (!handlerDispatched_)
    {
        ctx.outbound = ctx.inbound;
        ctx.outbound.type = "echo";
        ctx.hasOutbound = true;
    }

    // ---- 有回执就组帧入队（走 OutboundTask 体系） ----
    if (ctx.hasOutbound && !enqueueDataOrClose(codec_.encode(ctx.outbound)))
        return false;

    // ---- handler 要求关闭连接：发 Close 帧并标记 CloseConnection ----
    if (!ctx.keepConnection)
    {
        state_ = WsSessionState::Closing;  // 进入关闭流程，run() 下一轮会退出
        enqueueOutbound(
            WebSocketCodec::encodeClose(WsCloseCode::Normal, "handler close"),
            OutboundCompletion::CloseConnection);  // writerLoop 发完即触发 fd_close
        return false;
    }
    return true;
}

/**
 * @brief 会话主协程：WebSocket 帧收发主循环
 * @return Task<void>，C++20 协程
 *
 * 通俗解释：管家的"一天工作"——
 *   ① 上岗先查连接表（getConn），没了就直接退场；
 *   ② 向 SessionManager 登记自己（uid != 0 时）；
 *   ③ while(state==Open) 循环：decode → NeedMore 时 recvSocket 非阻塞读 →
 *     仍不够则 co_await ReadAwaiter 挂起等事件 → 收到完整帧按 opcode 分流；
 *   ④ Close 帧做关闭握手；Ping 回 Pong；Pong 续期；分片帧重组；数据帧交业务；
 *   ⑤ 任何需要关闭的情况发 Close + CloseConnection 回调，co_return 退场。
 *
 * @note 出站发送全部走 enqueueOutbound → OutboundQueue → writerLoop，不直接写 fd。
 */
Task<void> WebSocketSession::run()
{
    // ---- 上岗：先查连接，没了就直接退场 ----
    auto *bootConn = getConn();
    if (!bootConn)
    {
        state_ = WsSessionState::Closed;
        co_return;
    }
    // ---- 向 SessionManager 登记自己（匿名 uid==0 不登记） ----
    if (manager_ && uid_ != 0)
        manager_->registerSession(
            uid_, shared_from_this(), reactor_->reactorIndex(), key_);

    // ---- 主循环：state==Open 时持续收发帧 ----
    while (state_ == WsSessionState::Open)
    {
        // 每轮循环开头重新查连接表——协程挂起期间连接可能被其他线程关闭
        auto *conn = getConn();
        if (!conn || conn->state.closed)
        {
            state_ = WsSessionState::Closed;
            co_return;
        }

        // ---- 第一步：尝试从读缓冲解析一个完整帧 ----
        WsFrame frame;
        auto dr = codec_.decode(conn->readBuffer, parser_, frame);
        if (dr == WsDecodeResult::NeedMore)
        {
            // ---- 缓冲不够：先非阻塞 recvSocket 读一次（可能已有数据到达） ----
            const auto recvState = reactor_->recvSocket(key_.fd);
            if (recvState == RecvState::CLOSED)
            {
                state_ = WsSessionState::Closed;
                co_return;  // 对端关了，直接退场
            }
            // recv 后连接可能已被关闭，重新查表
            conn = getConn();
            if (!conn || conn->state.closed)
            {
                state_ = WsSessionState::Closed;
                co_return;
            }
            // ---- 再解析一次：recv 到的字节可能刚好凑齐一帧 ----
            dr = codec_.decode(conn->readBuffer, parser_, frame);
            if (dr == WsDecodeResult::NeedMore)
            {
                // 对端已发 FIN 但帧不完整：直接关，等不出后续数据了
                if (conn->state.peerClosed)
                {
                    reactor_->fd_close(
                        key_.fd,
                        "websocket peer closed incomplete frame",
                        CoroutineRole::Main);
                    state_ = WsSessionState::Closed;
                    co_return;
                }
                // ---- 仍然不够：挂起等 EPOLLIN 事件 ----
                co_await ReadAwaiter(reactor_, key_);
                continue;  // 被调度器恢复后回到 while 开头重新查连接、重新解析
            }
        }

        // ---- 协议错误：回 Close(ProtocolError) 并关闭连接 ----
        if (dr == WsDecodeResult::Error)
        {
            state_ = WsSessionState::Closing;
            enqueueOutbound(
                WebSocketCodec::encodeClose(
                    WsCloseCode::ProtocolError, "protocol error"),
                OutboundCompletion::CloseConnection);
            co_return;
        }

        // Parser 已完整消费一帧：归还读缓冲额度，后续 ReadAwaiter 才能重新武装 EPOLLIN。
        afterFrameConsumed(*conn);

        // ---- Close 帧：先校验 payload 语义，再完成关闭握手 ----
        if (dr == WsDecodeResult::Closed || frame.opcode == WsOpcode::Close)
        {
            WsCloseInfo closeInfo;
            const auto closeResult = parseWebSocketClosePayload(
                frame.payload, closeInfo);
            if (closeResult != WsClosePayloadResult::Ok)
            {
                state_ = WsSessionState::Closing;
                const auto responseCode =
                    closeResult == WsClosePayloadResult::InvalidUtf8
                        ? WsCloseCode::InvalidPayload
                        : WsCloseCode::ProtocolError;
                enqueueOutbound(
                    WebSocketCodec::encodeClose(responseCode),
                    OutboundCompletion::CloseConnection);
                co_return;
            }

            // 只在 Open 状态回 Close（避免重复回）；Closing 状态直接退场
            if (state_ == WsSessionState::Open)
            {
                state_ = WsSessionState::Closing;
                const auto response = closeInfo.hasCode
                    ? WebSocketCodec::encodeClose(closeInfo.code, closeInfo.reason)
                    : WebSocketCodec::encodeClose();
                enqueueOutbound(
                    response,
                    OutboundCompletion::CloseConnection);  // 发完即关
            }
            co_return;
        }

        // ---- Ping 帧：回 Pong（payload 回显），并刷新活跃时间 ----
        if (frame.opcode == WsOpcode::Ping)
        {
            reactor_->touchActivity(key_.fd);
            if (!enqueueDataOrClose(WebSocketCodec::encodePong(frame.payload)))
                co_return;
            continue;
        }

        // ---- Pong 帧：收到心跳回应，刷新活跃时间（waitingPong 由 timer 逻辑处理） ----
        if (frame.opcode == WsOpcode::Pong)
        {
            reactor_->touchActivity(key_.fd);
            continue;
        }

        // ---- 数据帧交给独立组装器：校验顺序、限制总大小、完成分片重组 ----
        WsFrame completeMessage;
        const auto assembly = messageAssembler_.consume(
            std::move(frame), completeMessage);
        if (assembly == WsAssemblyResult::Incomplete)
            continue;
        if (assembly == WsAssemblyResult::ProtocolError)
        {
            state_ = WsSessionState::Closing;
            enqueueOutbound(
                WebSocketCodec::encodeClose(
                    WsCloseCode::ProtocolError, "invalid fragment sequence"),
                OutboundCompletion::CloseConnection);
            co_return;
        }
        if (assembly == WsAssemblyResult::MessageTooBig)
        {
            state_ = WsSessionState::Closing;
            enqueueOutbound(
                WebSocketCodec::encodeClose(
                    WsCloseCode::MessageTooBig, "message too big"),
                OutboundCompletion::CloseConnection);
            co_return;
        }
        frame = std::move(completeMessage);

        // UTF-8 是“整条 Text 消息”的约束；分片可能把一个码点切开，必须重组后再验。
        if (frame.opcode == WsOpcode::Text &&
            !isValidWebSocketUtf8(frame.payload))
        {
            state_ = WsSessionState::Closing;
            enqueueOutbound(
                WebSocketCodec::encodeClose(WsCloseCode::InvalidPayload),
                OutboundCompletion::CloseConnection);
            co_return;
        }

        // ---- 完整消息到手：Reactor 填单，Worker 做业务，完成后回 Reactor 发结果 ----
        reactor_->touchActivity(key_.fd);
        const auto handlerStart =
            startAppMessage(frame.opcode, std::move(frame.payload));
        if (handlerStart == WsHandlerStartResult::Closed)
        {
            state_ = WsSessionState::Closed;
            co_return;
        }
        if (handlerStart == WsHandlerStartResult::Overloaded)
        {
            state_ = WsSessionState::Closing;
            (void)enqueueOutbound(
                WebSocketCodec::encodeClose(
                    WsCloseCode::TryAgainLater, "handler queue full"),
                OutboundCompletion::CloseConnection);
            co_return;
        }

        // 一条连接同一时刻只允许一个业务任务：保持消息顺序，也让 Context 无需加锁。
        co_await ExecuteAwaiter(reactor_, key_);
        if (!getConn())
        {
            state_ = WsSessionState::Closed;
            co_return;
        }
        if (!finishAppMessage())
            co_return;
    }
}
