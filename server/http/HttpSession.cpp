#include "HttpSession.h"
#include "server/SubReactor/SubReactor.h"

Task<HttpRequest> HttpSession::readRequest()
{
    HttpRequest req;
    while (true)
    {
        reactor->recvSocket(conn->fd);
        if (conn->state.closed)
        {
            co_return {};
        }
        if (reactor->parseOneRequest(req, *conn))
        {
            co_return req;
        }
        co_await ReadAwaiter(reactor, conn->fd);
    }
}
Task<HttpResponse *> HttpSession::execute(HttpRequest &req)
{
    co_return 
}
Task<void> HttpSession::send(HttpResponse *resp)
{
    pendingResponse p;
    p.resp = resp;

    while (true)
    {
        auto state = reactor->sendBody(conn->fd, p);
        switch (state)
        {
        case SEND_OK:

            responsePool.release(resp);

            co_return;

        case SEND_AGAIN:

            co_await WriteAwaiter(reactor, conn->fd);

            break;

        default:

            reactor->fd_close(conn->fd, "send error", true);

            co_return;
        }
    }
}
Task<void> HttpSession::run()
{
    while (conn && !conn->state.closed)
    {
        auto req = co_await readRequest();
        if (conn->state.closed)
            break;
        HttpResponse *resp = co_await execute(req);
        if (conn->state.closed)
            break;

        // auto resp=router.handle(req);
        co_await send(resp);
        // co_await ReadEventAwaiter(reactor,conn);
        // while(parseOneRequest())
        // {
        //     execute();
        // }

        // co_await WritreEventAwaiter(reactor,conn);
        // while(sendResponse()){

        // }
    }
    co_return;
}