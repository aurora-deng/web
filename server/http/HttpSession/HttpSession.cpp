// ============================================================
// 文件名：HttpSession.cpp
// 所属模块：server/http/HttpSession —— HTTP 会话主协程实现
// ------------------------------------------------------------
// 【生活比喻：车间主任的一天工作日志】
// 本文件是 HttpSession 的实现，把 HTTP 请求-响应的完整生命周期串成协程。
// run() 是唯一根协程，直接掌管 READ / EXECUTE / SENT 三类挂起点；
// readRequest / startHandler / queueResponse 只同步推进状态，不再返回嵌套 Task。
// 出站发送走 OutboundTask + OutboundQueue 体系，Session 不持有 sender。
// ------------------------------------------------------------
// 关键技术点（初学者重点理解）：
//   1. 【协程挂起恢复】读不够 co_await ReadAwaiter，执行 handler co_await ExecuteAwaiter，
//      发响应 co_await SendCompletionAwaiter——每次挂起都让出线程，事件就绪后恢复。
//   2. 【出站零 sender】queueResponse 把响应包成 OutboundTask::http 入 OutboundQueue，
//      由 SubReactor 的 writerLoop + TransportWriter 搬运字节，协程只等完成通知。
//   3. 【WebSocket 交接】升级时 handoffWebSocket 创建 WebSocketSession 并 adopt 进调度器，
//      旧 HTTP 协程 co_return 退出，新 WS 协程接管 fd。
// ============================================================
#include "HttpSession.h"
#include "server/CoroutineScheduler/AWaiter.h"
#include "server/SubReactor/SubReactor.h"
#include "server/Executor/Executor.h"
#include "server/Route/Router.h"
#include "server/session/SessionFactory.h"
#include "server/websocket/WebSocketHandshake/WebSocketHandshake.h"

/**
 * @brief 构造函数：绑定连接身份、所属 reactor，并用 router 初始化 codec_
 * @param key 连接身份（fd + connId）
 * @param r 所属 SubReactor
 * @param router 路由表，传给 codec_ 供 dispatch 时路由分发
 */
HttpSession::HttpSession(ConnectionKey key, SubReactor *r, Router &router)
    : reactor(r),
      key_(key),
      codec_(router)
{
}

HttpSession::~HttpSession()
{
    requestHandlerStop();
    releasePendingResponse();
}

void HttpSession::requestHandlerStop() noexcept
{
    (void)handlerStopSource_.request_stop();
}

/**
 * @brief 通过 ConnectionKey 安全查找当前连接对象
 * @return 存在返回 Connection 指针；连接已销毁返回 nullptr
 * @note 协程挂起期间 fd 可能被关闭并复用，每次恢复都要用 fd+connId 重新查表。
 */
Connection *HttpSession::getConn()
{
    return reactor->findConnection(key_.fd, key_.connId);
}

void HttpSession::releasePendingResponse() noexcept
{
    if (!context_.response)
        return;
    responsePool.release(context_.response);
    context_.response = nullptr;
}

/**
 * @brief 发送完成后的收尾：更新事件、Keep-Alive 续连或关连接
 * @note 出站发送由 writerLoop 完成后回调到这里。Keep-Alive 则更新事件继续读，
 *       否则直接 fd_close 关连接。同时检查内存水位，解除读暂停。
 */
bool HttpSession::afterSend()
{
    auto *connPtr = getConn();
    if (!connPtr)
        return false;
    auto &conn_after_loop = *connPtr;

    // ---- 内存水位检查：积压数据已降到一半以下，解除读暂停 ----
    if (conn_after_loop.state.pauseByMemory &&
        conn_after_loop.readBuffer.readableBytes() < kMaxPendingReadBytes / 2)
    {
        conn_after_loop.state.pauseByMemory = false;
    }

    conn_after_loop.state.wantWrite = false;
    if (keepAlive_)
    {
        reactor->updateEvent(key_.fd);   // Keep-Alive：重新注册读事件，继续下一轮
        return true;
    }
    reactor->fd_close(key_.fd, "keepalive false", CoroutineRole::Main);
    state = SessionState::CLOSED;
    return false;
}

/**
 * @brief 读取并尝试解析一个请求（两段式：先解析已有缓冲，不够再 recv）
 * @return 读取结果（COMPLETE/NEED_MORE/CLOSED/ERROR/TOO_LARGE）
 *
 * 通俗解释：车间主任先看传送带上有没有完整包裹（解析已有缓冲），没有就让卡车再送一车
 *   （recv），再试一次。两段式提高半包命中效率，避免无谓的系统调用。
 */
RequestReadResult HttpSession::readRequest()
{
    auto conn = getConn();
    if (!conn || conn->state.closed)
        return RequestReadResult::CLOSED;

    // ---- 第一段：先尝试解析缓冲区里已有的数据 ----
    state = SessionState::PARSING;
    auto parseState = codec_.decode(
        conn->readBuffer, parser_, context_.request, keepAlive_);

    if (parseState == PARSE_OK)
    {
        if (conn->state.peerClosed)
            keepAlive_ = false;
        conn->pendingBytes = conn->readBuffer.readableBytes();
        if (conn->pendingBytes < kMaxPendingReadBytes)
            conn->state.pauseByMemory = false;
        return RequestReadResult::COMPLETE;
    }

    if (parseState == PARSE_ERROR)
        return RequestReadResult::ERROR;

    // ---- 第二段：缓冲不够，recv 一批新数据再解析 ----
    state = SessionState::READING;
    const auto recvState = reactor->recvSocket(key_.fd);
    if (recvState == RecvState::CLOSED)
        return RequestReadResult::CLOSED;

    conn = getConn();
    if (!conn || conn->state.closed)
        return RequestReadResult::CLOSED;

    state = SessionState::PARSING;
    parseState = codec_.decode(
        conn->readBuffer, parser_, context_.request, keepAlive_);
    if (parseState == PARSE_OK)
    {
        if (conn->state.peerClosed)
            keepAlive_ = false;
        conn->pendingBytes = conn->readBuffer.readableBytes();
        if (conn->pendingBytes < kMaxPendingReadBytes)
            conn->state.pauseByMemory = false;
        return RequestReadResult::COMPLETE;
    }

    if (parseState == PARSE_ERROR)
        return RequestReadResult::ERROR;

    // 读缓冲被限流暂停（积压太多），返回 TOO_LARGE 让上层关连接
    if (recvState == RecvState::PAUSED)
        return RequestReadResult::TOO_LARGE;

    if (conn->state.peerClosed)
    {
        reactor->fd_close(key_.fd, "peer closed with incomplete request", CoroutineRole::Main);
        return RequestReadResult::CLOSED;
    }

    return RequestReadResult::NEED_MORE;
}

/**
 * @brief 构造 WebSocket 升级握手响应（101 Switching Protocols）
 * @return true 101 响应已装好；false 不是升级请求或握手非法（已装 400 响应）
 *
 * 通俗解释：业务 handler 调过 acceptWebSocket() 后，这里校验请求是否符合 WebSocket
 *   升级规范（Upgrade: websocket + Sec-WebSocket-Key 等）。合法就填 101 响应，
 *   非法就填 400 错误响应。
 */
bool HttpSession::prepareWebSocketUpgrade()
{
    if (!context_.webSocketAccepted)
        return false;

    // ---- 校验是否为 WebSocket 升级请求 ----
    if (!WebSocketHandshake::isUpgradeRequest(context_.request))
    {
        if (context_.response)
            responsePool.release(context_.response);
        context_.response = responsePool.acquire();
        context_.response->status = 400;
        context_.response->statusText = "Bad Request";
        context_.response->keepAlive = false;
        context_.response->text("WebSocket upgrade required");
        context_.webSocketAccepted = false;
        return false;
    }

    // ---- 校验 Sec-WebSocket-Key 等握手字段 ----
    auto hs = WebSocketHandshake::validate(context_.request);
    if (!hs.ok)
    {
        if (context_.response)
            responsePool.release(context_.response);
        context_.response = responsePool.acquire();
        context_.response->status = 400;
        context_.response->statusText = "Bad Request";
        context_.response->keepAlive = false;
        context_.response->text(hs.failReason.empty() ? "Bad WebSocket handshake" : hs.failReason);
        context_.webSocketAccepted = false;
        return false;
    }

    // ---- 校验通过：填 101 Switching Protocols 响应 ----
    if (context_.response)
        responsePool.release(context_.response);
    context_.response = responsePool.acquire();
    WebSocketHandshake::fillResponse(*context_.response, hs);
    return true;
}

/**
 * @brief 创建 WebSocketSession 并交接 Connection::session
 * @return true 交接成功，本 HTTP 协程随后 co_return；false 交接失败
 *
 * 通俗解释：101 响应发完后，本 HTTP 车间主任"下班"，把 fd 交给新来的 WebSocket
 *   车间主任（WebSocketSession）。通过 SessionFactory 创建 WS 会话，启动其 run()
 *   协程并 adopt 进调度器，旧 HTTP 协程随即退出。
 */
bool HttpSession::handoffWebSocket()
{
    auto *conn = getConn();
    if (!conn || conn->state.closed)
        return false;

    // ---- 从查询参数取 uid（WebSocket 用户标识）----
    uint64_t wsUid = 0;
    if (auto it = context_.request.querryParams.find("uid"); it != context_.request.querryParams.end())
    {
        try
        {
            wsUid = std::stoull(it->second);
        }
        catch (...)
        {
            wsUid = 0;
        }
    }

    // ---- 初始化 WS 心跳计时器，重置事件状态 ----
    conn->timer.wsHeartbeat = true;
    conn->timer.waitingPong = false;
    conn->timer.lastActiveSec = 0;
    reactor->touchActivity(key_.fd);

    conn->state.wantWrite = false;
    reactor->updateEvent(key_.fd);

    // ---- 通过 SessionFactory 创建 WebSocketSession ----
    auto *factory = reactor->sessionFactory();
    if (!factory)
        return false;

    auto ws = factory->createWebSocketSession(key_, reactor, wsUid);
    if (!ws)
        return false;

    // ---- 交接：替换 Connection::session，启动 WS 协程并 adopt 进调度器 ----
    conn->session = ws;
    auto task = ws->run();
    auto h = task.release();
    conn->slot(CoroutineRole::Main).handle = h;
    reactor->scheduler().adopt(
        key_.fd, key_.connId, CoroutineRole::Main, h, ws);
    state = SessionState::CLOSED;   // 本 HTTP 协程标记结束，随后 co_return
    return true;
}

/**
 * @brief 检查业务是否请求了 WebSocket 升级，是则准备 101 响应
 * @return true 业务已接受升级且 101 响应就绪；false 未升级
 */
bool HttpSession::handleWebSocketUpgradeIfRequested()
{
    if (!context_.webSocketAccepted)
        return false;
    return prepareWebSocketUpgrade();
}

bool HttpSession::prepareSseStream()
{
    if (!context_.sseAccepted)
        return false;

    sseClientId_ = 0;
    const auto found = context_.request.querryParams.find("uid");
    if (found != context_.request.querryParams.end())
    {
        try
        {
            std::size_t parsed = 0;
            const auto value = std::stoull(found->second, &parsed);
            if (parsed == found->second.size() && value != 0)
                sseClientId_ = value;
        }
        catch (...)
        {
            sseClientId_ = 0;
        }
    }

    releasePendingResponse();
    context_.response = responsePool.acquire();
    context_.response->reset();
    if (sseClientId_ == 0)
    {
        context_.response->status = 400;
        context_.response->statusText = "Bad Request";
        context_.response->keepAlive = false;
        context_.response->text("SSE requires a positive uid query parameter");
        context_.sseAccepted = false;
        return false;
    }

    context_.response->status = 200;
    context_.response->statusText = "OK";
    context_.response->keepAlive = true;
    context_.response->setHeader(
        "Content-Type", "text/event-stream; charset=utf-8");
    context_.response->setHeader("Cache-Control", "no-cache");
    context_.response->setHeader("X-Accel-Buffering", "no");
    context_.response->beginChunkedStream();
    keepAlive_ = true;
    return true;
}

bool HttpSession::handleSseIfRequested()
{
    if (!context_.sseAccepted)
        return false;
    return prepareSseStream();
}

bool HttpSession::handoffSse()
{
    auto *conn = getConn();
    if (!conn || conn->state.closed || sseClientId_ == 0)
        return false;

    conn->timer.wsHeartbeat = false;
    conn->timer.waitingPong = false;
    conn->state.wantWrite = false;
    reactor->touchActivity(key_.fd);
    reactor->updateEvent(key_.fd);

    auto *factory = reactor->sessionFactory();
    if (!factory)
        return false;
    auto sse = factory->createSseSession(
        key_, reactor, sseClientId_);
    if (!sse)
        return false;

    conn->session = sse;
    auto task = sse->run();
    auto handle = task.release();
    conn->slot(CoroutineRole::Main).handle = handle;
    reactor->scheduler().adopt(
        key_.fd, key_.connId, CoroutineRole::Main, handle, sse);
    state = SessionState::CLOSED;
    return true;
}

/**
 * @brief 同步提交业务 handler；等待动作由唯一根协程 run() 执行
 */
HandlerStartResult HttpSession::startHandler()
{
    state = SessionState::EXECUTING;
    auto *conn = getConn();
    if (!conn || conn->state.closed)
    {
        state = SessionState::CLOSED;
        return HandlerStartResult::CLOSED;
    }

    // 每轮 handler 使用新的撤单源；截止时间使用 Executor 配置的 steady_clock 预算。
    handlerStopSource_ = std::stop_source{};
    context_.cancellation = HandlerCancellation{
        handlerStopSource_.get_token(),
        reactor->executor().handlerDeadline()};

    // ---- 提交到 HTTP 专用执行器线程池异步跑 handler ----
    const ConnectionKey key = key_;
    const bool submitted = reactor->executor().submit([this, key]()
                                                    {
        bool dispatched = false;
        bool failed = false;
        try
        {
            // 排队期间若连接已关或截止时间已到，不再进入业务代码。
            if (!context_.stopRequested())
                dispatched = this->codec_.dispatch(context_);
        }
        catch (...)
        {
            failed = true;
        }

        auto prepareError = [this](int status,
                                   const char *statusText,
                                   const char *body)
        {
            releasePendingResponse();
            context_.response = responsePool.acquire();
            context_.response->reset();
            context_.response->status = status;
            context_.response->statusText = statusText;
            context_.response->keepAlive = false;
            // 失败或超时必须取消业务留下的升级意图，否则 504/500 可能被后续 101 覆盖。
            context_.webSocketAccepted = false;
            context_.sseAccepted = false;
            context_.response->text(body);
        };

        // 截止时间优先于普通业务结果；忽略预算的 handler 返回后仍只能得到 504。
        if (context_.handlerDeadlineExceeded())
        {
            prepareError(504, "Gateway Timeout", "Handler Timeout");
        }
        else if (failed)
        {
            prepareError(500, "Internal Server Error", "Internal Server Error");
        }
        else if (!dispatched && context_.response)
        {
            releasePendingResponse();
        }
        reactor->notifyExecuteComplete(key.fd, key.connId);   // 唤醒主协程
    });
    if (submitted)
        return HandlerStartResult::SUBMITTED;

    // 线程池满：直接装 503 响应，根协程无需等待 Worker。
    releasePendingResponse();
    context_.response = responsePool.acquire();
    context_.response->status = 503;
    context_.response->statusText = "Service Unavailable";
    context_.response->keepAlive = false;
    context_.response->text("Service Unavailable");
    return HandlerStartResult::READY;
}

/**
 * @brief 同步编码并把响应移交给 OutboundTask
 * @return 非零 ticket；0 表示连接关闭或入队失败
 */
uint64_t HttpSession::queueResponse()
{
    auto *conn = getConn();
    if (!conn || conn->state.closed)
    {
        releasePendingResponse();
        state = SessionState::CLOSED;
        return 0;
    }
    auto *response = context_.response;
    if (!response)
    {
        // 没有响应对象说明分发失败，直接关连接
        reactor->fd_close(key_.fd, "request dispatch failed", CoroutineRole::Main);
        state = SessionState::CLOSED;
        return 0;
    }

    // ---- 合并 keepAlive：业务响应标志 && 当前连接标志 ----
    if (conn->state.peerClosed)
        keepAlive_ = false;
    response->keepAlive = response->keepAlive && keepAlive_;
    keepAlive_ = response->keepAlive;
    codec_.encode(*response);   // 序列化响应头进 HeaderBody_
    state = SessionState::WRITING;

    // ---- 预订出站票据（流量控制：队列满则 ticket==0）----
    const uint64_t ticket = reactor->reserveOutboundTicket(key_.fd);
    if (ticket == 0)
    {
        releasePendingResponse();
        state = SessionState::CLOSED;
        return 0;
    }

    // ---- 包成 OutboundTask 入 OutboundQueue，由 writerLoop 实际发送 ----
    auto task = OutboundTask::http(PooledHttpResponse(response), ticket);
    context_.response = nullptr;
    if (reactor->enqueueOutbound(key_.fd, std::move(task)) != EnqueueResult::Ok)
    {
        reactor->fd_close(
            key_.fd, "http outbound queue rejected", CoroutineRole::Main);
        state = SessionState::CLOSED;
        return 0;
    }
    return ticket;
}

/**
 * @brief 重置请求上下文与 keepAlive_，为下一轮请求清场
 * @note 每轮请求开始前调用，把档案袋清空、response 置空、keepAlive_ 复位，
 *       避免上一轮残留状态污染新请求。
 */
void HttpSession::resetRequestContext()
{
    releasePendingResponse();
    context_.request.reset();
    context_.route = nullptr;
    context_.params.clear();
    context_.handled = false;
    context_.Id = key_.connId;
    context_.fd = key_.fd;
    context_.webSocketAccepted = false;
    context_.sseAccepted = false;
    sseClientId_ = 0;
    context_.cancellation = HandlerCancellation{};
    keepAlive_ = true;
}

/**
 * @brief 会话主协程：请求-响应循环（override 自 Session 基类纯虚 run()）
 * @return Task<void>，C++20 协程
 *
 * 通俗解释：车间主任的"一天工作流程"——每轮先清场（resetRequestContext），再由同一个
 *   根协程推进读请求、等待后厨、准备升级、入队响应和等待发送完成。Keep-Alive 就继续
 *   下一轮，任一步失败就 co_return 收工；所有暂停点都能在 run() 中按顺序看到。
 */
Task<void> HttpSession::run()
{
    while (true)
    {
        if (!getConn())
        {
            state = SessionState::CLOSED;
            co_return;
        }

        resetRequestContext();

        // 阶段一：同一个根协程反复推进解析；数据不足时由它自己等待 READ。
        while (true)
        {
            const auto result = readRequest();
            if (result == RequestReadResult::COMPLETE)
                break;

            if (result == RequestReadResult::NEED_MORE)
            {
                co_await ReadAwaiter(reactor, key_);
                if (!getConn())
                {
                    state = SessionState::CLOSED;
                    co_return;
                }
                continue;
            }

            state = SessionState::CLOSED;
            if (result == RequestReadResult::ERROR ||
                result == RequestReadResult::TOO_LARGE)
            {
                reactor->fd_close(
                    key_.fd,
                    result == RequestReadResult::ERROR
                        ? "malformed HTTP request"
                        : "request exceeds read buffer limit",
                    CoroutineRole::Main);
            }
            co_return;
        }

        // 阶段二：同步提交业务；只有根协程等待 Worker 完成。
        const auto handlerResult = startHandler();
        if (handlerResult == HandlerStartResult::CLOSED)
            co_return;
        if (handlerResult == HandlerStartResult::SUBMITTED)
        {
            co_await ExecuteAwaiter(reactor, key_);
            if (!getConn())
            {
                releasePendingResponse();
                state = SessionState::CLOSED;
                co_return;
            }
        }

        // 阶段三：业务决定是否升级；响应统一入队后等待 ticket 真正发完。
        const bool upgrading = handleWebSocketUpgradeIfRequested();
        const bool streamingSse = !upgrading && handleSseIfRequested();
        const uint64_t ticket = queueResponse();
        if (ticket == 0)
            co_return;

        co_await SendCompletionAwaiter(reactor, key_, ticket);
        if (!getConn())
        {
            state = SessionState::CLOSED;
            co_return;
        }

        // 101 必须完全写出后才把 Main 槽交给 WebSocket 根协程。
        if (upgrading)
        {
            if (!handoffWebSocket())
                reactor->fd_close(
                    key_.fd,
                    "websocket handoff failed",
                    CoroutineRole::Main);
            state = SessionState::CLOSED;
            co_return;
        }

        if (streamingSse)
        {
            if (!handoffSse())
                reactor->fd_close(
                    key_.fd,
                    "sse handoff failed",
                    CoroutineRole::Main);
            state = SessionState::CLOSED;
            co_return;
        }

        if (!afterSend())
            co_return;
    }
}
