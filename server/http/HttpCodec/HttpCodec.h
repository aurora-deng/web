#pragma once
#ifndef HTTP_CODEC_H
#define HTTP_CODEC_H
#include"server/http/http.h"
#include"server/http/HttpParser/HttpParser.h"
struct Connection;
class HttpResponse;
class HttpCodec
{
public:
    HttpCodec()=default;
    ParseState decode(Connection &conn,HttpRequest &req);
    bool dispatch(RequestContext&);
    SendState encode(HttpResponse& resp);
private:
    Router router;
};

#endif