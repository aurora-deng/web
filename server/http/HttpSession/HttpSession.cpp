#include "HttpSession.h"
#include "server/SubReactor/SubReactor.h"

Connection *HttpSession::getConn()
{
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

bool HttpSession::execute(RequestContext &ctx)
{
    auto conn=getConn();

    if(!conn)
        return false;

    ctx.response=std::make_shared<HttpResponse>();
    // ctx.response=
    //     reactor->createResponse(
    //         ctx.request,
    //         conn->keepAlive
    //     );
    bool ok=reactor->router.handle(ctx);
    if(!ok&&!ctx.response)
    {
        ctx.response->status=500;
    }
    conn->responses.push_back(*ctx.response);

    return true;
}

bool HttpSession::readRequest(RequestContext &ctx)
{

    auto conn = getConn();
    if (!conn || conn->state.closed)
        return false;

    auto parseState = reactor->parseOneRequest(ctx.request, *conn);
    if (parseState == PARSE_OK)
        return true;
    if (parseState == PARSE_ERROR)
        return false;

    const auto recvState = reactor->recvSocket(fd);
    if (recvState == RecvState::CLOSED)
        return false;

    conn = getConn();
    if (!conn || conn->state.closed)
        return false;

    parseState = reactor->parseOneRequest(ctx.request, *conn);
    if (parseState == PARSE_OK)
        return true;
    if (parseState == PARSE_ERROR)
        return false;

    if (recvState == RecvState::PAUSED)
    {
        reactor->fd_close(fd, "request exceeds read buffer limit", true);
        return false;
    }
}

Task<void> HttpSession::run()
{

    while (true)
    {
        RequestContext ctx;
        ctx.session=this;
        ctx.fd=fd;
        auto conn = getConn();
        if (!conn || conn->state.closed)
            co_return;
        state=SessionState::READ_REQUEST;
        while (true)
        {
            if(readRequest(ctx))break;
            co_await ReadAwaiter(reactor, fd);
        }

         state=SessionState::EXECUTE;
         
        if(!reactor->codec.dispatch(ctx))
            co_return;

        state=SessionState::SEND_RESPONSE;

        while (true)
        {
            conn = getConn();
            if (!conn || conn->state.closed)
            {
                responsePool.release(ctx.response);
                ctx.response=nullptr;
                co_return;
            }
            ctx.response->buildHeader();
            auto state = reactor->sendBody(fd, *ctx.response);
            switch (state)
            {
            case SEND_OK:

                responsePool.release(ctx.response);
                afterSend();
                break;

            case SEND_AGAIN:

                co_await WriteAwaiter(reactor, fd);

                continue;

            default:
                responsePool.release(ctx.response);
                ctx.response=nullptr;
                reactor->fd_close(fd, "send error", true);

                co_return;
            }
            break;
        }
    }
}