// =============================================================================
// 文件名：Router.cpp
// 职责比喻：路由实现 —— 导购台的工作流程
//
// 【整体比喻】
// 本文件实现 Router 的"导购"流程：
//   1. 顾客（请求）到来 → 先过中间件安检（dispatchMiddleware）；
//   2. 安检通过 → 先查静态路由表（matchStatic，O(1) 哈希查找，最快）；
//   3. 静态没命中 → 遍历动态路由表（matchDynamic，支持 :param 参数提取）；
//   4. 都没命中 → 返回 404 Not Found。
//
// 【路径切分】
// "/api/users/:id" 和 "/api//users//id" 都切成 ["api","users","id"]，
// 忽略重复/首尾斜杠，动态参数按片段位置比较，避免字符串前缀误匹配。
//
// 关键技术点（初学者重点理解）：
//   1. 静态优先：静态路由用 map O(1) 查找，比动态遍历快，先查静态。
//   2. 动态参数提取：:id 片段匹配任意值，存入 ctx.params 供 handler 使用。
//   3. 中间件洋葱模型：递归 lambda + next() 回调，链式执行。
//   4. 对象池复用：404 响应从 responsePool 获取，用完归还。
// =============================================================================
#include "Router.h"

/**
 * @brief 把路径按 '/' 切分成片段数组
 * @param path 原始路径，如 "/api/users/:id"
 * @return 片段数组，如 ["api","users",":id"]，忽略空片段（重复/首尾斜杠）
 *
 * 【设计动机】
 * 路由模板和请求路径使用相同切分规则，忽略重复/首尾斜杠产生的空片段；
 * 动态参数因此按片段位置比较，而不是做容易误匹配的字符串前缀判断。
 */
static std::vector<std::string> splitPath(const std::string &path)
{
    std::vector<std::string> res;
    std::stringstream ss(path);
    std::string item;

    while (std::getline(ss, item, '/'))
    {
        if (!item.empty())      // 忽略空片段（首尾/重复斜杠产生的）
            res.push_back(item);
    }

    return res;
}

/**
 * @brief 注册 GET 路由
 * @param path 路径模板（可含 :param 动态参数）
 * @param handler 匹配成功时调用的处理函数
 *
 * 【流程】
 * 1. 构造 RouteEntry，切分路径为 parts；
 * 2. 检查 parts 是否含 ':' 开头的动态参数；
 * 3. 含动态参数 → 存入 dynamicRoutes（vector，遍历匹配）；
 * 4. 纯静态 → 存入 staticRoutes（map，O(1) 查找）。
 */
void Router::GET(const std::string &path, Handler handler)
{

    RouteEntry entry;
    entry.method = "GET";
    entry.path = path;
    entry.parts = splitPath(path);
    entry.handler = std::move(handler);
    bool dynamic = false;
    // ---- 检查是否含动态参数（:param 形式） ----
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
        dynamicRoutes[entry.method].push_back(std::move(entry));   // 动态路由存 vector
    }
    else
    {
        staticRoutes[entry.method + " " + path] = std::move(entry);  // 静态路由存 map
    }
}

/**
 * @brief 注册 POST 路由（逻辑与 GET 完全相同，仅 method="POST"）
 */
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

/**
 * @brief 注册中间件
 * @param mw 中间件回调
 * 简单 push_back 到中间件列表，handle 时按顺序执行
 */
void Router::use(Middleware mw)
{
    middlewares.push_back(std::move(mw));
}

/**
 * @brief 在动态路由列表中匹配请求
 * @param ctx 请求上下文（含请求路径）
 * @param methodRoutes 当前方法的动态路由列表
 * @return true 表示匹配并处理成功
 *
 * 【匹配规则】
 * 1. 切分请求路径为片段；
 * 2. 遍历每条路由，片段数不同则跳过；
 * 3. 逐片段比较：:param 匹配任意值并存入 ctx.params，普通片段需完全相等；
 * 4. 全部片段匹配则调 handler，返回 true。
 */
bool Router::matchRoute(RequestContext &ctx, const std::vector<RouteEntry> &methodRoutes)
{

    // 路由匹配
    auto reqParts = splitPath(ctx.request.path);

    for (auto &route : methodRoutes)
    {

        // ---- 片段数不同，直接跳过（如 /a/b vs /a/b/c） ----
        if (route.parts.size() != reqParts.size())
            continue;

        bool match = true;

        ctx.params.clear();   // 清空上次匹配的参数

        for (size_t i = 0; i < route.parts.size(); i++)
        {
            const auto &rp = route.parts[i];   // 路由模板片段

            const auto &qp = reqParts[i];      // 请求路径片段

            // 动态参数
            // ---- :param 片段：匹配任意值，存入 params ----
            if (!rp.empty() && rp[0] == ':')
            {
                ctx.params[rp.substr(1)] = qp;   // substr(1) 去掉 ':'，如 "id"
            }
            else if (rp != qp)
            {
                // ---- 普通片段：必须完全相等 ----
                match = false;
                break;
            }
        }

        if (match)
        {
            ctx.route = &route;   // 记录命中的路由条目
            if (route.handler(ctx))
            {
                ctx.handled = true;
                return true;
            }
        }
    }

    return false;
}

/**
 * @brief 处理请求的统一入口
 * @param ctx 请求上下文
 * @return true 表示已生成响应
 *
 * 【处理流程】
 * 1. 中间件链：全部 next() 通过才继续，否则直接返回（中间件可能已生成响应）；
 * 2. 静态路由匹配：O(1) 查找，最快；
 * 3. 动态路由匹配：遍历 vector，支持 :param；
 * 4. 都没命中：生成 404 响应。
 */
bool Router::handle(RequestContext &ctx)
{

    if (!dispatchMiddleware(ctx))
        return true;      // 中间件未全部通过（可能已生成响应），直接返回
    if (matchStatic(ctx))
        return true;      // 静态路由命中
    if (matchDynamic(ctx))
        return true;      // 动态路由命中

    make404(ctx);         // 都没命中，生成 404

    return true;

    return ctx.handled;   // 死代码（上面已 return），保留以防逻辑扩展
}

/**
 * @brief 执行中间件链（洋葱模型）
 * @param ctx 请求上下文
 * @return true 表示所有中间件都 next() 通过，可继续路由匹配
 *
 * 【中间件通俗解释】
 * 中间件像洋葱的层：请求从外到内依次穿过每层。每层中间件可以：
 *   - 做预处理（如日志、鉴权）；
 *   - 调 next() 进入下一层；
 *   - 不调 next() 则中断，不进入路由匹配。
 * 用递归 lambda 实现：index 记录当前执行到第几个中间件，next() 调用下一个。
 */
bool Router::dispatchMiddleware(RequestContext &ctx)
{
    size_t index = 0;                    // 当前中间件索引
    bool continueToRoute = false;        // 是否所有中间件都通过
    std::function<void()> next;
    // Middleware
    next = [&]()
    {
        if (index < middlewares.size())
        {
            auto &mw = middlewares[index++];
            mw(ctx, next);   // 执行当前中间件，传入 next 供其调用来继续
        }
        else
        {
            continueToRoute = true;   // 所有中间件都通过，可以进入路由匹配
        }
    };
    next();   // 启动中间件链
    return continueToRoute;
}

/**
 * @brief 静态路由匹配（O(1) 哈希查找）
 * @param ctx 请求上下文
 * @return true 表示命中
 *
 * 【设计动机】静态路由用 "METHOD path" 作 key，unordered_map O(1) 查找，
 * 比动态路由遍历快很多，所以优先查静态。
 */
bool Router::matchStatic(RequestContext &ctx)
{
    // 静态
    std::string key =
        ctx.request.method + " " + ctx.request.path;   // 构造查找 key，如 "GET /api/users"

    auto it =
        staticRoutes.find(key);

    if (it != staticRoutes.end())
    {

        ctx.route = &it->second;        // 记录命中的路由条目
        ctx.handled = it->second.handler(ctx);  // 执行处理函数
        ctx.handled = true;
        return true;
    }
    return false;
}

/**
 * @brief 动态路由匹配（遍历 vector，支持 :param）
 * @param ctx 请求上下文
 * @return true 表示命中
 *
 * 【设计动机】动态路由含 :param，无法用哈希直接查找，只能遍历。
 * 用 vector 而非 map：顺序存储、缓存友好、支持参数匹配。
 */
bool Router::matchDynamic(RequestContext &ctx)
{
    // 动态
    auto methodIt = dynamicRoutes.find(ctx.request.method);
    if (methodIt == dynamicRoutes.end())
    {
        return false;   // 当前方法没有动态路由
    }
    auto &methodRoutes =
        methodIt->second;

    return matchRoute(ctx, methodRoutes);   // 委托给 matchRoute 遍历匹配
}

/**
 * @brief 生成 404 Not Found 响应
 * @param ctx 请求上下文
 * 从对象池获取 HttpResponse，设置 404 状态码和 "Not Found" 文本
 */
void Router::make404(RequestContext &ctx)
{
    if (ctx.response)
        responsePool.release(ctx.response);
    ctx.response = responsePool.acquire();   // 从对象池获取响应对象

    ctx.response->status = 404;
    ctx.response->text("Not Found");
    ctx.handled = true;
}
