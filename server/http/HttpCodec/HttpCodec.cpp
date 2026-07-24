#include "HttpCodec.h"
#include "server/SubReactor/SubReactor.h"
ParseState HttpCodec::decode(Connection &conn, HttpRequest &req)
{
    // 完成流式解析
    auto state = conn.parser.parse(conn.readBuffer, req);

    if (state != PARSE_OK)
        return state;

    conn.keepAlive = conn.parser.keepAlive();

    conn.parser.reset();

    return PARSE_OK;
}

bool HttpCodec::dispatch(RequestContext &ctx)
{
    if(ctx.response==nullptr)
    {
        ctx.response=responsePool.acquire();
    }
    return router.handle(ctx);
}
