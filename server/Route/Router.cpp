// =============================================================================
// 文件名：Router.cpp
// 职责：注册、解析并执行 HTTP 路由与中间件。
//
// Phase 10 把“找到路由”和“运行路由”拆成两步。HttpSession 可以先只读地
// resolve()，据此决定把请求留在 Reactor，还是投递给 Worker。路由表在 Runtime
// 启动时 freeze()，之后所有 Reactor 只并发读取，不会遇到容器扩容导致的悬空指针。
// =============================================================================
#include "Router.h"

#include <sstream>
#include <stdexcept>

namespace
{
std::vector<std::string> splitPath(const std::string &path)
{
    std::vector<std::string> result;
    std::stringstream stream(path);
    std::string part;
    while (std::getline(stream, part, '/'))
    {
        if (!part.empty())
            result.push_back(std::move(part));
    }
    return result;
}

bool isDynamic(const RouteEntry &entry)
{
    for (const auto &part : entry.parts)
    {
        if (!part.empty() && part.front() == ':')
            return true;
    }
    return false;
}

// 一条请求只在栈上保存这个轻量状态。MiddlewareNext 仅携带状态指针和函数指针，
// 避免旧实现为每个请求构造递归 std::function<void()> 所产生的分配和引用计数。
struct MiddlewareChainState
{
    const std::vector<Middleware> *middlewares{};
    RequestContext *context{};
    std::size_t index{};
    bool reachedRoute{};
};

void dispatchNext(void *opaque)
{
    auto &state = *static_cast<MiddlewareChainState *>(opaque);
    if (state.index < state.middlewares->size())
    {
        const auto &middleware = (*state.middlewares)[state.index++];
        middleware(*state.context, MiddlewareNext{&state, dispatchNext});
        return;
    }
    state.reachedRoute = true;
}
} // namespace

void Router::GET(const std::string &path, Handler handler, RouteOptions options)
{
    if (frozen())
        throw std::logic_error("cannot register GET route after Router::freeze()");

    RouteEntry entry;
    entry.method = "GET";
    entry.path = path;
    entry.parts = splitPath(path);
    entry.handler = std::move(handler);
    entry.execution = options.execution;

    if (isDynamic(entry))
        dynamicRoutes[entry.method].push_back(std::move(entry));
    else
        staticRoutes[entry.method][path] = std::move(entry);
}

void Router::POST(const std::string &path, Handler handler, RouteOptions options)
{
    if (frozen())
        throw std::logic_error("cannot register POST route after Router::freeze()");

    RouteEntry entry;
    entry.method = "POST";
    entry.path = path;
    entry.parts = splitPath(path);
    entry.handler = std::move(handler);
    entry.execution = options.execution;

    if (isDynamic(entry))
        dynamicRoutes[entry.method].push_back(std::move(entry));
    else
        staticRoutes[entry.method][path] = std::move(entry);
}

void Router::PATCH(const std::string &path, Handler handler, RouteOptions options)
{
    if (frozen())
        throw std::logic_error("cannot register PATCH route after Router::freeze()");

    RouteEntry entry;
    entry.method = "PATCH";
    entry.path = path;
    entry.parts = splitPath(path);
    entry.handler = std::move(handler);
    entry.execution = options.execution;

    if (isDynamic(entry))
        dynamicRoutes[entry.method].push_back(std::move(entry));
    else
        staticRoutes[entry.method][path] = std::move(entry);
}

void Router::use(Middleware middleware)
{
    if (frozen())
        throw std::logic_error("cannot register middleware after Router::freeze()");
    middlewares.push_back(std::move(middleware));
}

bool Router::resolveStatic(RequestContext &ctx) const
{
    const auto method = staticRoutes.find(ctx.request.method);
    if (method == staticRoutes.end())
        return false;

    const auto route = method->second.find(ctx.request.path);
    if (route == method->second.end())
        return false;

    ctx.route = &route->second;
    return true;
}

bool Router::resolveDynamicRoute(
    RequestContext &ctx,
    const std::vector<RouteEntry> &methodRoutes) const
{
    const auto requestParts = splitPath(ctx.request.path);
    for (const auto &route : methodRoutes)
    {
        if (route.parts.size() != requestParts.size())
            continue;

        bool matches = true;
        ctx.params.clear();
        for (std::size_t index = 0; index < route.parts.size(); ++index)
        {
            const auto &routePart = route.parts[index];
            const auto &requestPart = requestParts[index];
            if (!routePart.empty() && routePart.front() == ':')
            {
                ctx.params[routePart.substr(1)] = requestPart;
            }
            else if (routePart != requestPart)
            {
                matches = false;
                break;
            }
        }

        if (matches)
        {
            ctx.route = &route;
            return true;
        }
    }

    ctx.params.clear();
    return false;
}

bool Router::resolveDynamic(RequestContext &ctx) const
{
    const auto method = dynamicRoutes.find(ctx.request.method);
    return method != dynamicRoutes.end() && resolveDynamicRoute(ctx, method->second);
}

ExecutionPolicy Router::resolve(RequestContext &ctx) const
{
    ctx.route = nullptr;
    ctx.params.clear();
    ctx.routeResolved = true;

    if (resolveStatic(ctx) || resolveDynamic(ctx))
        return ctx.route->execution;

    // 404 仍走 Worker：未知路径也可能经过日志、鉴权等用户中间件，不能擅自在
    // Reactor 上运行尚未审计的代码。
    return ExecutionPolicy::Worker;
}

bool Router::dispatchMiddleware(RequestContext &ctx)
{
    MiddlewareChainState state{&middlewares, &ctx, 0, false};
    dispatchNext(&state);
    return state.reachedRoute;
}

bool Router::handle(RequestContext &ctx)
{
    if (!ctx.routeResolved)
        (void)resolve(ctx);

    if (!dispatchMiddleware(ctx))
        return true;

    if (ctx.route)
    {
        ctx.handled = ctx.route->handler(ctx);
        if (ctx.handled)
            return true;
    }

    make404(ctx);
    return true;
}

void Router::make404(RequestContext &ctx)
{
    if (!ctx.response)
        ctx.response = responsePool.acquire();
    else
        ctx.response->reset();
    ctx.response->status = 404;
    ctx.response->text("Not Found");
    ctx.handled = true;
}
