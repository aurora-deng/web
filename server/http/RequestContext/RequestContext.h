#pragma once
#ifndef EEQUEST_CONTEXT_H
#define EEQUEST_CONTEXT_H
#include<cstring>
#include "server/http/http.h"
struct Connection;
class HttpSession;
struct RouteEntry;

struct  RequestContext
{
    // http请求
    HttpRequest request;
    // http响应
    HttpResponse* response=nullptr;
    // 当前链接
    int fd;
    // 当前会话
    HttpSession* session=nullptr;
    // 当前路由器
    const RouteEntry *route=nullptr;

    std::unordered_map<std::string ,std::string> params;
    
    bool handled=false;

    uint64_t Id=0;
    // 获得参数
    std::string param(const std::string& key);
    // 查询参数
    std::string querry(const std::string &key);
};

#endif