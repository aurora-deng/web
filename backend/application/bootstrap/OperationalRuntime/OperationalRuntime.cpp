#include "backend/application/bootstrap/OperationalRuntime/OperationalRuntime.h"
#include "backend/application/bootstrap/BootstrapSettings/BootstrapSettings.h"

#include "ProjectBuild.h"
#include "backend/server/Runtime/ServerRuntime.h"
#include "backend/server/ops/OperationalMetrics.h"
#include "backend/server/security/AuthToken.h"
#include "backend/server/security/FixedWindowRateLimiter.h"
#include "backend/server/observability/logger/logger.h"
#include "backend/application/observability/BusinessMetrics/BusinessMetrics.h"
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
#include "backend/application/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"
#endif
#include <unordered_set>
#include <cctype>
#include <unistd.h>
namespace webserver::bootstrap {
namespace {
std::unordered_set<std::string> commaSeparatedSet(const char *raw)
{
    std::unordered_set<std::string> values;
    if (!raw)
        return values;
    std::string_view input(raw);
    std::size_t start = 0;
    while (start <= input.size())
    {
        const auto end = input.find(',', start);
        auto item = input.substr(start, end == std::string_view::npos
                                           ? input.size() - start
                                           : end - start);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front())))
            item.remove_prefix(1);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back())))
            item.remove_suffix(1);
        if (!item.empty())
            values.emplace(item);
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return values;
}

bool protectedPath(std::string_view path)
{
    return path == "/ws" || path == "/events" ||
           path.starts_with("/events/") || path == "/events-status" ||
           path == "/delivery-metrics" || path == "/metrics" ||
           path == "/admin";
}

bool operationsPath(const HttpRequest &request)
{
    return request.path == "/metrics" || request.path == "/delivery-metrics" ||
           request.path == "/events-status" ||
           (request.method == "POST" && request.path.starts_with("/events/"));
}

void reject(RequestContext &ctx, int status, std::string statusText,
            const std::string &message)
{
    ctx.response->status = status;
    ctx.response->statusText = std::move(statusText);
    ctx.response->json("{\"error\":\"" + message + "\"}");
    ctx.response->setHeader("Cache-Control", "no-store");
}

bool queryUserMatches(const RequestContext &ctx, std::uint64_t userId)
{
    const auto found = ctx.request.querryParams.find("uid");
    if (found == ctx.request.querryParams.end() || found->second.empty())
        return true;
    try
    {
        std::size_t parsed = 0;
        const auto supplied = std::stoull(found->second, &parsed);
        return parsed == found->second.size() && supplied == userId;
    }
    catch (...)
    {
        return false;
    }
}
}
struct OperationalRuntime::Impl {
    bool productionMode;
    std::string secret=config::RuntimeConfig::environment("WEB_AUTH_SECRET");
    std::shared_ptr<security::AuthToken> authTokens=std::make_shared<security::AuthToken>(secret);
    std::shared_ptr<const std::unordered_set<std::string>> allowedOrigins=std::make_shared<const std::unordered_set<std::string>>(commaSeparatedSet(std::getenv("WEB_ALLOWED_ORIGINS")));
    std::shared_ptr<ops::OperationalMetrics> operationalMetrics=std::make_shared<ops::OperationalMetrics>();
    std::shared_ptr<security::FixedWindowRateLimiter> rateLimiter=std::make_shared<security::FixedWindowRateLimiter>(size("WEB_REQUESTS_PER_MINUTE",120,1,1000000),std::chrono::minutes(1),size("WEB_RATE_LIMIT_IDENTITIES",65536,1,1000000));
    explicit Impl(bool p):productionMode(p){if(p&&secret.size()<32)throw std::invalid_argument("WEB_PRODUCTION_MODE requires WEB_AUTH_SECRET with at least 32 bytes");if(p&&allowedOrigins->empty())throw std::invalid_argument("WEB_PRODUCTION_MODE requires WEB_ALLOWED_ORIGINS");}
};
OperationalRuntime::OperationalRuntime(bool p):impl_(std::make_unique<Impl>(p)){}
OperationalRuntime::~OperationalRuntime()=default;
std::shared_ptr<ops::OperationalMetrics> OperationalRuntime::metrics()const{return impl_->operationalMetrics;}
const std::string& OperationalRuntime::authSecret()const{return impl_->secret;}
void OperationalRuntime::install(ServerRuntime&server,phase11::transport::Phase11AuthHttp*authentication,std::function<bool()>phase11Ready,std::function<std::string()>phase11MetricsText,bool businessSchemaVerified){
    const auto authTokens=impl_->authTokens;const auto allowedOrigins=impl_->allowedOrigins;
    const auto operationalMetrics=impl_->operationalMetrics;const auto rateLimiter=impl_->rateLimiter;const bool productionMode=impl_->productionMode;
    (void)authentication;
        server.router().use([operationalMetrics](RequestContext &ctx, auto next)
                            {
        operationalMetrics->recordHttpRequest();
        (void)ctx; // 压测模式下 LOG_HTTP 是空宏，仍显式标记参数已使用。
        LOG_HTTP(ctx.request.method + " " + ctx.request.path);
        next(); });

        // 统一安检：HTTP、WebSocket 握手和 SSE 握手先在这里得到可信身份。
        // 长连接接管后只复制 ctx 中已校验的 userId，不能信任客户端随意填写的 uid。
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
        auto *phase11RequestAuth = authentication;
#endif
        server.router().use(
            [authTokens, allowedOrigins, operationalMetrics, rateLimiter,
             productionMode
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
             , phase11RequestAuth
#endif
             ](RequestContext &ctx, auto next)
            {
        if (!protectedPath(ctx.request.path))
        {
            next();
            return;
        }

        bool phase11Identity = false;
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
        if (phase11RequestAuth &&
            (ctx.request.path == "/ws" || ctx.request.path == "/events"))
        {
            try
            {
                const auto restored = phase11RequestAuth->authorizeRequest(ctx);
                ctx.authenticated = true;
                ctx.authenticatedUserId = restored.user.id;
                ctx.authenticatedTenant = "phase11";
                phase11Identity = true;
            }
            catch (const webserver::phase11::ApplicationError &error)
            {
                if (error.code() == webserver::phase11::ErrorCode::Unavailable)
                {
                    reject(ctx, 503, "Service Unavailable",
                           "phase11 authentication is busy");
                    return;
                }
                // 若还配置了 Phase 9 Bearer Token，允许它继续作为运维/兼容入口。
            }
        }
#endif

        if (!phase11Identity && !authTokens->enabled())
        {
            // Phase 11 已启用时，/ws 与 /events 必须携带有效的 Phase 11 Session。
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
            if (phase11RequestAuth &&
                (ctx.request.path == "/ws" || ctx.request.path == "/events"))
            {
                operationalMetrics->recordAuthenticationRejected();
                reject(ctx, 401, "Unauthorized", "valid phase11 session required");
                return;
            }
#endif
            // 学习模式没有 HMAC 密钥时保留旧版 /admin 中间件契约：
            // 缺少 legacy token 仍要在路由前返回纯文本 401，防止回归测试被 404 掩盖。
            if (ctx.request.path == "/admin" &&
                ctx.request.headers.find("token") == ctx.request.headers.end())
            {
                ctx.response->status = 401;
                ctx.response->statusText = "Unauthorized";
                ctx.response->text("Unauthorized");
                return;
            }
            next();
            return;
        }

        if (!phase11Identity)
        {
            const auto authenticated = authTokens->authenticateHeaders(
                ctx.request.headers);
            if (!authenticated)
            {
                operationalMetrics->recordAuthenticationRejected();
                reject(ctx, 401, "Unauthorized", "valid bearer token required");
                return;
            }
            ctx.authenticated = true;
            ctx.authenticatedUserId = authenticated.identity->userId;
            ctx.authenticatedTenant = authenticated.identity->tenant;
        }

        if (operationsPath(ctx.request) && ctx.authenticatedTenant != "ops")
        {
            operationalMetrics->recordAuthenticationRejected();
            reject(ctx, 403, "Forbidden", "operations tenant required");
            return;
        }

        if ((ctx.request.path == "/ws" || ctx.request.path == "/events") &&
            !queryUserMatches(ctx, ctx.authenticatedUserId))
        {
            operationalMetrics->recordAuthenticationRejected();
            reject(ctx, 403, "Forbidden", "uid does not match authenticated identity");
            return;
        }

        // WebSocket 的 Origin 是浏览器替用户发来的站点身份证。生产模式必须存在；
        // 其他模式只要配置了白名单，就对携带 Origin 的浏览器请求做同样检查。
        const bool browserLongConnection =
            ctx.request.path == "/ws" || ctx.request.path == "/events";
        const bool authenticatedByCookie =
            ctx.request.headers.find("authorization") == ctx.request.headers.end() &&
            ctx.request.headers.find("cookie") != ctx.request.headers.end();
        const bool cookieStateChange =
            authenticatedByCookie && ctx.request.method != "GET" &&
            ctx.request.method != "HEAD";
        if (browserLongConnection || cookieStateChange)
        {
            const auto found = ctx.request.headers.find("origin");
            // Cookie 会被浏览器自动附带，因此任何使用 Cookie 的写请求都必须携带
            // 白名单 Origin；否则即使在开发模式，也会留下可被跨站页面利用的 CSRF 缺口。
            const bool missingRequired = (productionMode || cookieStateChange) &&
                                         found == ctx.request.headers.end();
            const bool untrusted = !allowedOrigins->empty() &&
                                   found != ctx.request.headers.end() &&
                                   !allowedOrigins->contains(found->second);
            if (missingRequired || untrusted)
            {
                operationalMetrics->recordOriginRejected();
                reject(ctx, 403, "Forbidden", "origin is not allowed");
                return;
            }
        }

        if (!rateLimiter->allow(ctx.authenticatedUserId))
        {
            operationalMetrics->recordRateLimited();
            reject(ctx, 429, "Too Many Requests", "identity request quota exceeded");
            ctx.response->setHeader("Retry-After", "60");
            return;
        }
        next(); });

        server.router().GET("/", [](RequestContext &ctx) -> bool
                            {
        ctx.response->html("<h1>hello</h1>");
        return true; }, RouteOptions{ExecutionPolicy::ReactorSafe});

        server.router().GET("/health/version", [businessSchemaVerified](RequestContext &ctx) -> bool {
            std::string version=webserver::build::versionJson;version.pop_back();
            version+=",\"schemaVerifiedAtStartup\":"+std::string(businessSchemaVerified?"true":"false")+"}";
            ctx.response->json(version);
            return true;
        });
        server.router().GET("/health/live", [](RequestContext &ctx) -> bool
                            {
        ctx.response->json("{\"status\":\"alive\"}");
        return true; }, RouteOptions{ExecutionPolicy::ReactorSafe});

        server.router().GET("/health/ready", [&server, phase11Ready](RequestContext &ctx) -> bool
                            {
        const bool ready = server.ready() && phase11Ready();
        if (!ready)
        {
            ctx.response->status = 503;
            ctx.response->statusText = "Service Unavailable";
        }
        ctx.response->json(ready
                               ? "{\"status\":\"ready\"}"
                               : "{\"status\":\"starting\"}");
        return true; });

        server.router().GET(
            "/metrics",
            [&server, operationalMetrics,
             phase11MetricsText](RequestContext &ctx) -> bool
            {
                const auto sse = server.sseManager().metrics();
                auto body = operationalMetrics->prometheus(
                    server.wsManager().onlineCount(),
                    server.sseManager().onlineCount(),
                    server.sseManager().historyEventCount());
                body += "# TYPE webserver_sse_published_total counter\n";
                body += "webserver_sse_published_total " +
                        std::to_string(sse.publishedEvents) + "\n";
                body += "webserver_sse_deliveries_total " +
                        std::to_string(sse.acceptedDeliveries) + "\n";
                body += "webserver_sse_replayed_total " +
                        std::to_string(sse.replayedEvents) + "\n";
                body += "webserver_sse_replay_gaps_total " +
                        std::to_string(sse.replayGaps) + "\n";
                body += phase11MetricsText();
                body += webserver::business_metrics::text();
                ctx.response->text(body);
                ctx.response->setHeader(
                    "Content-Type", "text/plain; version=0.0.4; charset=utf-8");
                ctx.response->setHeader("Cache-Control", "no-store");
                return true;
            });

        // 动态路由：:id 是占位符，框架自动提取 URL 实际值到 ctx.params
        server.router().GET("/user/:id", [](RequestContext &ctx) -> bool
                            {
        ctx.response->text(ctx.params.at("id"));
        return true; }, RouteOptions{ExecutionPolicy::ReactorSafe});

        // 分块传输（chunked）示例：把响应拆成多块发送，适合动态生成内容
        server.router().GET("/stream1", [](RequestContext &ctx) -> bool
                            {
        ctx.response->beginChunked();
        ctx.response->writeChunk("hello");
        ctx.response->writeChunk(" world");
        ctx.response->endChunked();
        return true; });

        // /stream2 的 sleep(1) 在 Executor Worker 线程执行，不会阻塞 Reactor
        // 这验证了"慢业务丢给线程池、主循环保持敏捷"的异步设计
        server.router().GET("/stream2", [](RequestContext &ctx) -> bool
                            {
        ctx.response->beginChunked();
        for (int i = 0; i < 10; i++)
        {
            if (ctx.stopRequested())
                break;
            ctx.response->writeChunk("hello\n");
            sleep(1);
        }
        ctx.response->endChunked();
        return true; });

        // 静态文件服务：用 sendfile 零拷贝发送 SVG 图片
        // 支持 Range 请求（断点续传），失败时返回 404
        server.router().GET("/logo", [](RequestContext &ctx) -> bool
                            {
        const std::string path = "./static/logo.svg";
        ctx.response->setHeader("Content-Type", "image/svg+xml");
        if (!ctx.response->sendfile(path, ctx.request, ctx.request.range))
        {
            ctx.response->status = 404;
            ctx.response->statusText = "Not Found";
            ctx.response->text("Not Found");
        }
        return true; });


}
}
