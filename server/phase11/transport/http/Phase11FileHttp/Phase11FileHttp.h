#pragma once

#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/business/BusinessInterfaces/BusinessInterfaces.h"
#include "server/phase11/runtime/BoundedExecutor/BoundedExecutor.h"
#include "server/phase11/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"

#include <functional>
#include <future>
#include <type_traits>

class Router;

namespace webserver::phase11::transport
{

/** 文件 API 是单独的交通岗：处理有界分片、封存、权限下载和文件消息。 */
class Phase11FileHttp final
{
public:
    Phase11FileHttp(Phase11AuthHttp &authentication,
                    IFileBusiness &files, IChatBusiness &chat,
                    BoundedExecutor &fileExecutor,
                    std::function<void()> outboxCommitted = {})
        : authentication_(authentication), files_(files), chat_(chat),
          fileExecutor_(fileExecutor),
          outboxCommitted_(std::move(outboxCommitted))
    {
    }

    void registerRoutes(Router &router);

private:
    template<class Operation>
    auto runFile(Operation &&operation)
        -> std::invoke_result_t<std::decay_t<Operation>>
    {
        using Result = std::invoke_result_t<std::decay_t<Operation>>;
        auto completion = std::make_shared<std::promise<Result>>();
        auto result = completion->get_future();
        if (!fileExecutor_.submit(
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
                        else completion->set_value(operation());
                    }
                    catch (...) { completion->set_exception(std::current_exception()); }
                }))
            throw ApplicationError(ErrorCode::Unavailable,
                                   "file service is busy");
        return result.get();
    }

    Phase11AuthHttp &authentication_;
    IFileBusiness &files_;
    IChatBusiness &chat_;
    BoundedExecutor &fileExecutor_;
    std::function<void()> outboxCommitted_;
};

} // namespace webserver::phase11::transport
