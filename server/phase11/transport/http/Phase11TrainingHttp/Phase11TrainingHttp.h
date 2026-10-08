#pragma once

#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/business/BusinessInterfaces/BusinessInterfaces.h"
#include "server/phase11/runtime/BoundedExecutor/BoundedExecutor.h"
#include "server/phase11/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"

#include <cstddef>
#include <future>
#include <string>
#include <type_traits>
#include <utility>

class Router;

namespace webserver::phase11::transport
{

struct Phase11TrainingHttpConfig
{
    std::size_t maximumRequestBytes = 96U * 1024U;
    std::string consentVersion = "phase22-training-consent-v1";
};

/** HTTP 只接收明确同意和脱敏后的内容；审核与数据集导出保留为运维操作。 */
class Phase11TrainingHttp final
{
public:
    Phase11TrainingHttp(Phase11AuthHttp &authentication,
                        ITrainingBusiness &training,
                        DatabaseExecutor &databaseExecutor,
                        Phase11TrainingHttpConfig config = {})
        : authentication_(authentication), training_(training),
          databaseExecutor_(databaseExecutor), config_(std::move(config))
    {
    }

    void registerRoutes(Router &router);

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
                                   "training service is busy");
        return result.get();
    }

    Phase11AuthHttp &authentication_;
    ITrainingBusiness &training_;
    DatabaseExecutor &databaseExecutor_;
    Phase11TrainingHttpConfig config_;
};

} // namespace webserver::phase11::transport
