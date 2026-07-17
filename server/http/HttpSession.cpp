#include "HttpSession.h"
#include "server/SubReactor/SubReactor.h"

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

Task<void> HttpSession::run()
{
    auto getConn = [&]() -> Connection *
    {
        auto it = reactor->conns.find(fd);
        if (it == reactor->conns.end())
            return nullptr;

        return it->second.get();
    };
    while (true)
    {

        auto conn = getConn();
        if (!conn || conn->state.closed)
            co_return;
        HttpRequest req;
        while (true)
        {
            auto conn = getConn();
            if (!conn || conn->state.closed)
                co_return;

            auto parseState = reactor->parseOneRequest(req, *conn);
            if (parseState == PARSE_OK)
                break;
            if (parseState == PARSE_ERROR)
                co_return;

            const auto recvState = reactor->recvSocket(fd);
            if (recvState == RecvState::CLOSED)
                co_return;

            conn = getConn();
            if (!conn || conn->state.closed)
                co_return;

            parseState = reactor->parseOneRequest(req, *conn);
            if (parseState == PARSE_OK)
                break;
            if (parseState == PARSE_ERROR)
                co_return;

            if (recvState == RecvState::PAUSED)
            {
                reactor->fd_close(fd, "request exceeds read buffer limit", true);
                co_return;
            }
            co_await ReadAwaiter(reactor, fd);
        }

        conn = getConn();
        if (!conn || conn->state.closed)
            co_return;
        HttpResponse *resp = reactor->createResponse(req,conn->keepAlive);

        while (true)
        {
            conn = getConn();
            if (!conn || conn->state.closed)
            {
                responsePool.release(resp);
                co_return;
            }
            auto state = reactor->sendBody(fd, *resp);
            switch (state)
            {
            case SEND_OK:

                responsePool.release(resp);
                afterSend();
                break;

            case SEND_AGAIN:

                co_await WriteAwaiter(reactor, fd);

                continue;

            default:
                responsePool.release(resp);
                reactor->fd_close(fd, "send error", true);

                co_return;
            }
            break;
        }
    }
}