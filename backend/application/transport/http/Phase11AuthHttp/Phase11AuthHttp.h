#pragma once

#include "backend/application/business/ApplicationError/ApplicationError.h"
#include "backend/application/business/BusinessInterfaces/BusinessInterfaces.h"
#include "backend/application/runtime/BoundedExecutor/BoundedExecutor.h"

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
 * 路由注册时绑定有界 DatabaseExecutor，协议 Session 直接投递完整工作单并挂起。
 * PostgreSQL 与 Argon2id 在该执行器内顺序运行，通用 HTTP Worker 不再陪同等待。
 * 直接调用适配器的旧测试仍可使用同步入口；同池重入则直接执行，避免自等死锁。
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
        // 已在所属业务执行器上，直接执行，避免把任务再排给自己而死锁。
        if (databaseExecutor_.isCurrentThread()) return operation();
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
