#include "HttpCodec.h"
#include "server/SubReactor/SubReactor.h"
ParseState HttpCodec::decode(Connection &conn, HttpRequest &req)
{
    // 完成流式解析
     // parser 属于连接，可跨多次非阻塞读取保留状态。只有完整请求才同步 keep-alive 并 reset；
    // NEED_MORE 时重置会丢失半包进度，ERROR 时则由 Session 关闭连接，无需尝试复用解析器。
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
