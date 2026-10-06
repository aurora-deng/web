#pragma once

#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/business/BusinessInterfaces/BusinessInterfaces.h"
#include "server/phase11/runtime/BoundedExecutor/BoundedExecutor.h"

#include <chrono>
#include <future>
#include <string>
#include <type_traits>
#include <utility>

class Router;
struct RequestContext;

namespace webserver::phase11::transport
{

struct Phase11AuthHttpConfig
{
    std::string sessionCookieName = "phase11_session";
    bool secureCookie = true;
    std::chrono::seconds sessionLifetime = std::chrono::hours(24);
    std::size_t maximumJsonBytes = 4096;

    // 只有服务器确实位于可信反向代理后方时才填写，例如 "x-real-ip"。默认不相信
    // 客户端可以随意伪造的转发头，来源限流退化为单实例全局来源桶，账号桶仍独立工作。
    std::string trustedClientIpHeader;
};

/**
 * 把 HTTP 请求翻译成 AuthService 调用。
 *
 * Handler 本身运行在通用 HTTP Worker；真正的 PostgreSQL 与 Argon2id 操作再进入独立、
 * 有界的 DatabaseExecutor。当前 HTTP 框架尚无“异步返回响应”的 continuation，因此
 * HTTP Worker 会等待结果，但 Reactor 永远不会等待数据库，队列满时立即返回 503。
 */
class Phase11AuthHttp final
{
public:
    Phase11AuthHttp(IAuthBusiness &auth, DatabaseExecutor &databaseExecutor,
                    Phase11AuthHttpConfig config = {});

    void registerRoutes(Router &router);

    /**
     * 从 HttpOnly Cookie 恢复身份；requireCsrf=true 时同时校验 X-CSRF-Token。
     * 其他 Phase 11 HTTP 适配器和 WebSocket/SSE 握手共用这一扇“安检门”。
     */
    [[nodiscard]] RestoredSession authorizeRequest(
        const RequestContext &context, bool requireCsrf = false);

private:
    template<class Operation>
    auto runDatabase(Operation &&operation)
        -> std::invoke_result_t<std::decay_t<Operation>>
    {
        using Result = std::invoke_result_t<std::decay_t<Operation>>;
        auto completion = std::make_shared<std::promise<Result>>();
        auto result = completion->get_future();
        if (!databaseExecutor_.submit(
                [completion,
                 operation = std::forward<Operation>(operation)]() mutable
                {
                    try
                    {
                        if constexpr (std::is_void_v<Result>)
                        {
                            operation();
                            completion->set_value();
                        }
                        else
                            completion->set_value(operation());
                    }
                    catch (...)
                    {
                        completion->set_exception(std::current_exception());
                    }
                }))
            throw ApplicationError(ErrorCode::Unavailable,
                                   "authentication service is busy");
        return result.get();
    }

    IAuthBusiness &auth_;
    DatabaseExecutor &databaseExecutor_;
    Phase11AuthHttpConfig config_;
};

} // namespace webserver::phase11::transport
