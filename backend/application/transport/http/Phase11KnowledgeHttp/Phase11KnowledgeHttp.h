#pragma once

#include "backend/application/business/ApplicationError/ApplicationError.h"
#include "backend/application/business/BusinessInterfaces/BusinessInterfaces.h"
#include "backend/application/runtime/BoundedExecutor/BoundedExecutor.h"
#include "backend/application/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"

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
        // 已在所属业务执行器上，直接执行，避免把任务再排给自己而死锁。
        if (knowledgeExecutor_.isCurrentThread()) return operation();
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
