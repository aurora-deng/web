#pragma once
#ifndef HTTP_CODEC_H
#define HTTP_CODEC_H
#include"server/http/http.h"
#include"server/http/HttpParser/HttpParser.h"
struct Connection;
class HttpResponse;
class Router;
class RequestContext;
// HttpCodec 是 Reactor 与 HTTP 语义之间的适配层：decode 驱动连接私有解析器，
// dispatch 则准备响应对象并进入路由。集中这条边界后，Reactor 无需了解路由和对象池细节，
// 后续增加协议版本或替换分发策略时也有明确扩展点。

class HttpCodec
{
public:
    explicit HttpCodec(Router &router):router(router){}
    ParseState decode(
        Buffer &buffer,
        HttpParser &parser,
        HttpRequest &req,
        bool &keepAlive);
    bool dispatch(RequestContext& ctx);
    // SendState encode(HttpResponse& resp);
private:
// Router 的生命周期由 main 保证长于所有 SubReactor；Codec 仅借用引用，不参与所有权。
    Router& router;
};

#endif