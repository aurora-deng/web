#pragma once

#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/business/BusinessInterfaces/BusinessInterfaces.h"
#include "server/phase11/runtime/BoundedExecutor/BoundedExecutor.h"
#include "server/phase11/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"

#include <cstddef>
#include <future>
#include <type_traits>
#include <utility>

class Router;

namespace webserver::phase11::transport
{

struct Phase11KnowledgeHttpConfig
{
    std::size_t maximumRequestBytes = 4U * 1024U * 1024U;
};

/**
 * 知识库 HTTP 适配器。
 *
 * 它像业务层前面的翻译员：Cookie/CSRF、路径和 JSON 在这里转换，文档权限、
 * 切片、索引与 Citation 规则仍由 IKnowledgeBusiness 决定。
 */
class Phase11KnowledgeHttp final
{
public:
    Phase11KnowledgeHttp(Phase11AuthHttp &authentication,
                         IKnowledgeBusiness &knowledge,
                         BoundedExecutor &knowledgeExecutor,
                         Phase11KnowledgeHttpConfig config = {})
        : authentication_(authentication), knowledge_(knowledge),
          knowledgeExecutor_(knowledgeExecutor), config_(config)
    {
    }

    void registerRoutes(Router &router);

private:
    template<class Operation>
    auto runKnowledge(Operation &&operation)
        -> std::invoke_result_t<std::decay_t<Operation>>
    {
        using Result = std::invoke_result_t<std::decay_t<Operation>>;
        auto completion = std::make_shared<std::promise<Result>>();
        auto result = completion->get_future();
        if (!knowledgeExecutor_.submit(
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
                                   "knowledge service is busy");
        return result.get();
    }

    Phase11AuthHttp &authentication_;
    IKnowledgeBusiness &knowledge_;
    BoundedExecutor &knowledgeExecutor_;
    Phase11KnowledgeHttpConfig config_;
};

} // namespace webserver::phase11::transport
