#include "HttpCodec.h"
#include "server/SubReactor/SubReactor.h"

ParseState HttpCodec::decode(
    Buffer &buffer,
    HttpParser &parser,
    HttpRequest &req,
    bool &keepAlive)
{
    // parser 属于 HttpSession，可跨多次非阻塞读取保留状态。Codec 不再依赖 Connection/SubReactor，
    // 因而协议层可以独立测试和复用。只有完整请求才同步 keep-alive 并 reset；
    // NEED_MORE 时重置会丢失半包进度，ERROR 时则由 Session 关闭连接，无需尝试复用解析器。
    auto state = parser.parse(buffer, req);

    if (state != PARSE_OK)
        return state;

    keepAlive = parser.keepAlive();
    parser.reset();

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
