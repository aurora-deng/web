#include "HttpSession.h"
#include "server/SubReactor/SubReactor.h"

Task<HttpRequest> HttpSession::readRequest()
{
    // co_return Task<HttpRequest>();
}

Task<HttpResponse *> HttpSession::execute(HttpRequest &req)
{
    co_return reactor->createResponse(req);
}
Task<void> HttpSession::send(HttpResponse *resp)
{

    while (true)
    {
        auto state = reactor->sendBody(fd, *resp);
        switch (state)
        {
        case SEND_OK:

            responsePool.release(resp);
            afterSend();
            co_return;

        case SEND_AGAIN:

            co_await WriteAwaiter(reactor, fd);

            break;

        default:

            reactor->fd_close(fd, "send error", true);

            co_return;
        }
    }
}

bool HttpSession::readSocket()
{
    return reactor->recvSocket(fd);
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
    
   
    conn_after_loop.state.wantWrite=false;
    if(conn_after_loop.keepAlive)
    {
        reactor->updateEvent(fd);
    }
    else
    {
        reactor->fd_close(fd,"keepalive false");
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
        if (!conn)
            co_return;
        HttpRequest req;
        // 不需要使用recv是由于希望一次await、一次事件、一次处理
        while (true)
        {

            if (readSocket())
            {
                co_return;
            }
            conn = getConn();
            if (!conn)
                co_return;

            if (!reactor->parseOneRequest(req, *conn))
                co_await ReadAwaiter(reactor, fd);
        }

        HttpResponse *resp = reactor->createResponse(req);
        if (conn->state.closed)
            break;

        // auto resp=router.handle(req);

        while (true)
        {
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

                reactor->fd_close(fd, "send error", true);

                co_return;
            }
            break;
        }
    }
}