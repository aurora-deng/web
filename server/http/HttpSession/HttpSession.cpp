#include "HttpSession.h"
#include "server/SubReactor/SubReactor.h"

Connection *HttpSession::getConn()
{
    // fd 只是查找键，不保证对象仍存在；关闭流程会先从 conns 删除对象，
    // 所有跨挂起点的调用都通过查表把“连接已消失”转化为 nullptr。
    auto it = reactor->conns.find(fd);
    if (it == reactor->conns.end())
        return nullptr;

    return it->second.get();
}
void HttpSession::afterSend()
{
    // 段错误修复处：循环结束后重新查找 conn，因为循环内可能已通过 fd_close 删除了 conn
    auto it = reactor->conns.find(fd);
    if (it == reactor->conns.end())
        return;
    // 重新获取 conn 引用（循环前的 conn 可能因 fd_close 而悬空）
    auto &conn_after_loop = *it->second;

    // 背压修复处：发送完响应后检查是否可以恢复读
    // 如果 readBuffer 可读数据降到水位线以下，恢复 pauseByMemory
    if (conn_after_loop.state.pauseByMemory &&
        conn_after_loop.readBuffer.readableBytes() < MAX_PENDING_BYTES / 2)
    {
        conn_after_loop.state.pauseByMemory = false;
    }

    conn_after_loop.state.wantWrite = false;
    if (conn_after_loop.keepAlive)
    {
        reactor->updateEvent(fd);
    }
    else
    {
        reactor->fd_close(fd, "keepalive false", true);
    }
}

// bool HttpSession::sendResponse(HttpResponse &resp)
// {
//     while (true)
//     {
//         auto conn = getConn();
//         if (!conn || conn->state.closed)
//         {
//             responsePool.release(&resp);
//             co_return false;
//         }
//         auto state = reactor->sendBody(fd, resp);
//         switch (state)
//         {
//         case SEND_OK:

//             responsePool.release(&resp);
//             afterSend();
//             co_return true;

//         case SEND_AGAIN:

//             co_await WriteAwaiter(reactor, fd);

//             break;
//             ;

//         default:
//             responsePool.release(&resp);
//             reactor->fd_close(fd, "send error", true);

//             co_return false;
//         }
//     }
// }

// bool HttpSession::execute(RequestContext &ctx)
// {
//     auto conn = getConn();

//     if (!conn)
//         return false;

//     ctx.response = std::make_shared<HttpResponse>();
//     // ctx.response=
//     //     reactor->createResponse(
//     //         ctx.request,
//     //         conn->keepAlive
//     //     );
//     bool ok = reactor->router.handle(ctx);
//     if (!ok && !ctx.response)
//     {
//         ctx.response->status = 500;
//     }
//     conn->responses.push_back(*ctx.response);

//     return true;
// }

RequestReadResult HttpSession::readRequest(RequestContext &ctx)
{
    // 先解析已有 Buffer，可处理上一次 recv 带来的粘包并避免无意义系统调用；
    // 只有 NEED_MORE 才 recv，再解析一次，把 Reactor 的底层状态映射成 Session 可处理的结果。
    auto conn = getConn();
    if (!conn || conn->state.closed)
        return RequestReadResult::CLOSED;

    auto parseState = reactor->parseOneRequest(ctx.request, *conn);
    if (parseState == PARSE_OK)
        return RequestReadResult::COMPLETE;
    if (parseState == PARSE_ERROR)
        return RequestReadResult::ERROR;

    const auto recvState = reactor->recvSocket(fd);
    if (recvState == RecvState::CLOSED)
        return RequestReadResult::CLOSED;

    conn = getConn();
    if (!conn || conn->state.closed)
        return RequestReadResult::CLOSED;

    parseState = reactor->parseOneRequest(ctx.request, *conn);
    if (parseState == PARSE_OK)
        return RequestReadResult::COMPLETE;
    if (parseState == PARSE_ERROR)
        return RequestReadResult::ERROR;

    if (recvState == RecvState::PAUSED)
        return RequestReadResult::TOO_LARGE;

    return RequestReadResult::NEED_MORE;
}

Task<void> HttpSession::run()
{
    // 每轮 RequestContext 都是请求级对象，避免 keep-alive 下参数、响应指针或 handled 状态串到下一请求。
    // Connection 指针不会跨 co_await 保存；每次恢复后重新查表是协程与连接关闭并存的关键边界。
    while (true)
    {
        RequestContext ctx;
        ctx.session = this;
        ctx.fd = fd;
        auto conn = getConn();
        if (!conn || conn->state.closed)
        {
            state = SessionState::CLOSED;
            co_return;
        }
        state = SessionState::READ_REQUEST;
        while (true)
        {
            const auto result = readRequest(ctx);
            if (result == RequestReadResult::COMPLETE)
                break;
            if (result == RequestReadResult::CLOSED)
            {
                state = SessionState::CLOSED;
                co_return;
            }
            if (result == RequestReadResult::ERROR ||
                result == RequestReadResult::TOO_LARGE)
            {
                state = SessionState::CLOSED;
                reactor->fd_close(fd,
                                  result == RequestReadResult::ERROR
                                      ? "malformed HTTP request"
                                      : "request exceeds read buffer limit",
                                  true);
                co_return;
            }
            // 当前没有完整请求时登记 READ 并让出执行权；EPOLLIN 到达后由 Reactor 统一恢复。
            co_await ReadAwaiter(reactor, fd);
        }

        state = SessionState::EXECUTE;
        const bool dispatched = reactor->codec.dispatch(ctx);
        auto *response = ctx.response;
        if (!dispatched || !response)
        {
            if (response)
                responsePool.release(response);
            ctx.response = nullptr;
            reactor->fd_close(fd, "request dispatch failed", true);
            state = SessionState::CLOSED;
            co_return;
        }
        conn = getConn();
        if (!conn || conn->state.closed)
        {
            responsePool.release(response);
            ctx.response = nullptr;
            state = SessionState::CLOSED;
            co_return;
        }
        // 响应只有在业务层和请求协议都允许时才能复用连接；任一方要求关闭都具有更高优先级。
        response->keepAlive = response->keepAlive && conn->keepAlive;
        response->buildHeader();
        state = SessionState::SEND_RESPONSE;
        while (true)
        {
            conn = getConn();
            if (!conn || conn->state.closed)
            {
                responsePool.release(ctx.response);
                ctx.response = nullptr;
                state = SessionState::CLOSED;
                co_return;
            }
            const auto sendState = reactor->sender.send(fd, *response);
            switch (sendState)
            {
            case SEND_OK:

                responsePool.release(response);
                ctx.response = nullptr;
                afterSend();
                break;

            case SEND_AGAIN:
                // 非阻塞发送遇到 EAGAIN 时保留 Body 内部消费偏移，等待 EPOLLOUT 后从原位置续发。
                co_await WriteAwaiter(reactor, fd);

                continue;

            default:
                responsePool.release(response);
                ctx.response = nullptr;
                reactor->fd_close(fd, "send error", true);
                state = SessionState::CLOSED;
                co_return;
            }
            break;
        }
    }
}