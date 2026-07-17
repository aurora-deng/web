#ifndef ROUTER_H
#define ROUTER_H
#include <functional>
#include <unordered_map>
#include <utility>
#include<vector>

#include "server/http/http.h"

struct Context
{
    HttpRequest req;
    HttpResponse resp;
    std::unordered_map<std::string ,std::string> params;
    std::string param(const std::string& key);
    std::string querry(const std::string &key);
};


using Middleware=std::function<bool(Context&)>;
using Handler = std::function<void(Context &)>;

struct RouteEntry
{
    std::string method;

    std::string path;
    
    std::vector<std::string> parts;

    Handler handler;
};


class Router
{
public:
    void GET(const std::string &path, Handler handler);

    void POST(const std::string &path, Handler handler);

    void use(Middleware mw);

    HttpResponse handle(HttpRequest &req) const;

private:
    // std::unordered_map<std::string, Handler> getRoutes;

    // std::unordered_map<std::string, Handler> postRoutes;
    // 优化：泛化使用router使得不需要频繁创建get等等
    // std::unordered_map<std::string, Handler> Routes;
    // 优化使用vector来快速的提高查询和实现顺序存储,降低map的开销,提高你命中率
    std::vector<RouteEntry> routes;
    std::vector<Middleware> middlewares;
};

#endif