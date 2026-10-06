// =============================================================================
// 文件名：Router.h
// 职责比喻：路由表 —— HTTP 请求的"导购台"
//
// 【整体比喻】
// 想象一家大商场，顾客（HTTP 请求）进来后不知道该去哪个柜台，先到导购台问一下。
// Router 就是这个导购台：它内部维护一张"URL → 处理函数"的映射表。
// 当请求到来时，Router 根据请求方法（GET/POST）和路径找到对应的处理回调（Handler），
// 执行它来生成响应。找不到就返回 404。
//
// 【静态路由 vs 动态路由】
//   - 静态路由：路径完全匹配，如 GET /api/users → 用 unordered_map O(1) 查找；
//   - 动态路由：路径含参数，如 GET /api/users/:id → 用 vector 遍历，:id 可匹配任意值，
//     参数存入 ctx.params["id"] = "123"。适合 RESTful 风格 URL。
//
// 【中间件（Middleware）】
// 中间件是请求处理前的"预处理流水线"，如日志记录、鉴权、CORS 等。
// Router::use() 注册中间件，按注册顺序链式执行，全部通过后才进入路由匹配。
//
// 关键技术点（初学者重点理解）：
//   1. 双数据结构：静态路由用 map（快），动态路由用 vector（支持参数匹配）。
//   2. 路径切分匹配：把 "/a/b/c" 切成 ["a","b","c"] 按片段比较，忽略首尾斜杠。
//   3. 中间件链：MiddlewareNext 用状态指针推进，保留 next() 语义且避免每请求闭包分配。
//   4. 对象池：HttpRequest/HttpResponse 用 ObjectPoll 复用，减少内存分配。
// =============================================================================
#ifndef ROUTER_H
#define ROUTER_H
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "server/http/http.h"
#include"server/ObjectPool/ObjectPool.h"
#include "server/http/RequestContext/RequestContext.h"

// 全局对象池声明：HttpRequest 和 HttpResponse 都池化复用
extern ObjectPoll<HttpRequest> requestPool;
extern ObjectPoll<HttpResponse> responsePool;

// struct Context
// {
//     HttpRequest req;
//     HttpResponse resp;
//     std::unordered_map<std::string ,std::string> params;
//     std::string param(const std::string& key);
//     std::string querry(const std::string &key);
// };

/** handler 的执行位置。默认 Worker 保留阻塞隔离；只有明确审计过的短任务才能 ReactorSafe。 */
enum class ExecutionPolicy
{
    Worker,
    ReactorSafe
};

struct RouteOptions
{
    ExecutionPolicy execution{ExecutionPolicy::Worker};
};

/**
 * @brief 不拥有状态的中间件 continuation。
 *
 * 旧版每次请求都会构造和复制 std::function<void()>。这里改成“状态指针 +
 * 普通函数指针”，仍保留 next() 洋葱语义，但调用本身不需要堆分配。
 */
class MiddlewareNext
{
public:
    using Callback = void (*)(void *);

    MiddlewareNext() = default;
    MiddlewareNext(void *state, Callback callback) noexcept
        : state_(state), callback_(callback) {}

    void operator()() const
    {
        if (callback_)
            callback_(state_);
    }

private:
    void *state_{nullptr};
    Callback callback_{nullptr};
};

// 中间件类型：接收请求上下文和轻量 next 回调，调 next() 继续下一个中间件
using Middleware = std::function<void(RequestContext &, MiddlewareNext)>;
// 处理器类型：接收请求上下文，返回是否已处理（true 表示已生成响应）
using Handler = std::function<bool(RequestContext &)>;

// =============================================================================
// RouteEntry：一条路由规则（方法 + 路径 + 处理器）
// =============================================================================
// 【RouteEntry 通俗解释】
// 一条路由规则就是"当 GET /api/users/:id 来时，调用这个函数"。
// parts 是路径切分后的片段数组，用于动态参数匹配。
// =============================================================================
struct RouteEntry
{
    uint64_t id = 0;                // 路由 ID（预留，调试/统计用）；默认初始化避免复制未定义值
    std::string method;             // HTTP 方法："GET" / "POST"

    std::string path;               // 原始路径模板，如 "/api/users/:id"

    std::vector<std::string> parts; // 路径切分片段：["api","users",":id"]，:id 是动态参数

    Handler handler;                // 匹配成功时调用的处理函数
    ExecutionPolicy execution{ExecutionPolicy::Worker};
};

// =============================================================================
// Router：路由表，管理所有路由规则和中间件
// =============================================================================
class Router
{
public:
    // 方法 → 动态路由列表的映射（如 "GET" → [RouteEntry, ...]）
    using MethodRoutes =
        std::unordered_map<
            std::string,
            std::vector<RouteEntry>>;

public:
    /**
     * @brief 注册 GET 路由
     * @param path 路径模板（可含 :param 动态参数）
     * @param handler 处理回调
     * 静态路由（无 :param）存入 staticRoutes map；动态路由存入 dynamicRoutes vector
     */
    void GET(const std::string &path, Handler handler, RouteOptions options = {});

    /**
     * @brief 注册 POST 路由（逻辑与 GET 相同，仅方法名不同）
     */
    void POST(const std::string &path, Handler handler, RouteOptions options = {});

    /** @brief 注册 PATCH 路由，供 REST 资源的部分更新使用。 */
    void PATCH(const std::string &path, Handler handler, RouteOptions options = {});

    /**
     * @brief 注册中间件
     * @param mw 中间件回调
     * 中间件按注册顺序执行，全部 next() 才进入路由匹配
     */
    void use(Middleware mw);

    /** 启动前冻结路由表；冻结后只允许并发读取，继续注册会抛出 logic_error。 */
    void freeze() noexcept { frozen_.store(true, std::memory_order_release); }
    [[nodiscard]] bool frozen() const noexcept
    {
        return frozen_.load(std::memory_order_acquire);
    }

    /** 只解析路由并保存 RouteEntry/动态参数，不运行中间件和 handler。 */
    ExecutionPolicy resolve(RequestContext &ctx) const;

    /**
     * @brief 在动态路由列表中匹配请求
     * @param ctx 请求上下文（含请求路径和方法）
     * @param methodRoutes 当前方法的动态路由列表
     * @return true 表示匹配并处理成功
     * 匹配成功时动态参数填入 ctx.params
     */
    /** 处理已解析请求；未提前 resolve 时会自行解析以保持旧调用兼容。 */
    bool handle(RequestContext &ctx);

private:
    /**
     * @brief 执行中间件链
     * @return true 表示所有中间件都 next() 通过，可以继续路由匹配
     * 【设计动机】用递归 lambda 实现 next() 控制流，类似 Express/Koa 的洋葱模型
     */
    bool dispatchMiddleware(RequestContext &ctx);

    /**
     * @brief 尝试静态路由匹配（O(1) 哈希查找）
     * @return true 表示命中静态路由
     */
    bool resolveStatic(RequestContext &ctx) const;

    /**
     * @brief 尝试动态路由匹配（遍历 vector，支持 :param 参数提取）
     * @return true 表示命中动态路由
     */
    bool resolveDynamic(RequestContext &ctx) const;

    bool resolveDynamicRoute(
        RequestContext &ctx,
        const std::vector<RouteEntry> &methodRoutes) const;

    /**
     * @brief 生成 404 Not Found 响应
     * 从对象池获取 HttpResponse，设置 404 状态和 "Not Found" 文本
     */
    void make404(RequestContext &ctx);
    // std::unordered_map<std::string, Handler> getRoutes;

    // std::unordered_map<std::string, Handler> postRoutes;
    // 优化：泛化使用router使得不需要频繁创建get等等
    // std::unordered_map<std::string, Handler> Routes;
    // 优化使用vector来快速的提高查询和实现顺序存储,降低map的开销,提高你命中率

    // 动态路由表：方法 → 路由列表（vector 顺序遍历，支持 :param）
    MethodRoutes dynamicRoutes;
    // 静态路由表：method → path → RouteEntry，查询时无需拼接临时 key
    std::unordered_map<
        std::string,
        std::unordered_map<std::string, RouteEntry>> staticRoutes;
    // 中间件列表：按注册顺序执行
    std::vector<Middleware> middlewares;
    std::atomic<bool> frozen_{false};

};

#endif
