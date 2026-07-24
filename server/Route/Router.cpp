#include "Router.h"

// 解析path
static std::vector<std::string> splitPath(const std::string &path)
{
    std::vector<std::string> res;
    std::stringstream ss(path);
    std::string item;

    while (std::getline(ss, item, '/'))
    {
        if (!item.empty())
            res.push_back(item);
    }

    return res;
}

void Router::GET(const std::string &path, Handler handler)
{

    RouteEntry entry;
    entry.method = "GET";
    entry.path = path;
    entry.parts = splitPath(path);
    entry.handler = std::move(handler);
    bool dynamic = false;
    for (auto &p : entry.parts)
    {
        if (!p.empty() && p[0] == ':')
        {
            dynamic = true;
            break;
        }
    }
    if (dynamic)
    {
        dynamicRoutes[entry.method].push_back(std::move(entry));
    }
    else
    {
        staticRoutes[entry.method + " " + path] = std::move(entry);
    }
}

void Router::POST(const std::string &path, Handler handler)
{
    RouteEntry entry;
    entry.method = "POST";
    entry.path = path;
    entry.parts = splitPath(path);
    entry.handler = std::move(handler);
    bool dynamic = false;
    for (auto &p : entry.parts)
    {
        if (!p.empty() && p[0] == ':')
        {
            dynamic = true;
            break;
        }
    }
    if (dynamic)
    {
        dynamicRoutes[entry.method].push_back(std::move(entry));
    }
    else
    {
        staticRoutes[entry.method + " " + path] = std::move(entry);
    }
}

void Router::use(Middleware mw)
{
    middlewares.push_back(std::move(mw));
}

bool Router::matchRoute(RequestContext &ctx, const std::vector<RouteEntry> &methodRoutes)
{

    // 路由匹配
    auto reqParts = splitPath(ctx.request.path);

    for (auto &route : methodRoutes)
    {

        if (route.parts.size() != reqParts.size())
            continue;

        bool match = true;

        ctx.params.clear();

        for (size_t i = 0; i < route.parts.size(); i++)
        {
            const auto &rp = route.parts[i];

            const auto &qp = reqParts[i];

            // 动态参数
            if (!rp.empty() && rp[0] == ':')
            {
                ctx.params[rp.substr(1)] = qp;
            }
            else if (rp != qp)
            {
                match = false;
                break;
            }
        }

        if (match)
        {
            ctx.route = &route;
            if (route.handler(ctx))
            {
                ctx.handled = true;
                return true;
            }
        }
    }

    return false;
}

bool Router::handle(RequestContext &ctx)
{

    if (!dispatchMiddleware(ctx))
        return true;
    if (matchStatic(ctx))
        return true;
    if (matchDynamic(ctx))
        return true;

    make404(ctx);

    return true;

    return ctx.handled;
}

bool Router::dispatchMiddleware(RequestContext &ctx)
{
    size_t index = 0;
    std::function<void()> next;
    // Middleware
    next = [&]()
    {
        if (index < middlewares.size())
        {
            auto &mw = middlewares[index++];
            mw(ctx, next);

            return;
        }
    };
    next();
    return ctx.response==nullptr;
}

bool Router::matchStatic(RequestContext &ctx)
{
    // 静态
    std::string key =
        ctx.request.method + " " + ctx.request.path;

    auto it =
        staticRoutes.find(key);

    if (it != staticRoutes.end())
    {

        ctx.route = &it->second;
        ctx.handled = it->second.handler(ctx);
        ctx.handled = true;
        return true;
    }
    return false;
}

bool Router::matchDynamic(RequestContext &ctx)
{
    // 动态
    auto methodIt = dynamicRoutes.find(ctx.request.method);
    if (methodIt == dynamicRoutes.end())
    {
        return false;
    }
    auto &methodRoutes =
        methodIt->second;

    return matchRoute(ctx, methodRoutes);
    
}

void Router::make404(RequestContext &ctx)
{
    ctx.response = responsePool.acquire();

    ctx.response->status = 404;
    ctx.response->text("Not Found");
    ctx.handled = true;
}
