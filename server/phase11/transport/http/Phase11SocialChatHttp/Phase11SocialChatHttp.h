#pragma once

#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/business/BusinessInterfaces/BusinessInterfaces.h"
#include "server/phase11/runtime/BoundedExecutor/BoundedExecutor.h"
#include "server/phase11/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"

#include <future>
#include <type_traits>
#include <utility>

class Router;

namespace webserver::phase11::transport
{

/** 好友、私聊、群聊的 HTTP 查询与管理入口；实时消息仍交给 WebSocket。 */
class Phase11SocialChatHttp final
{
public:
    Phase11SocialChatHttp(Phase11AuthHttp &authentication,
                          ISocialBusiness &social,
                          IChatBusiness &chat,
                          DatabaseExecutor &databaseExecutor)
        : authentication_(authentication), social_(social), chat_(chat),
          databaseExecutor_(databaseExecutor)
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
                                   "database service is busy");
        return result.get();
    }

    Phase11AuthHttp &authentication_;
    ISocialBusiness &social_;
    IChatBusiness &chat_;
    DatabaseExecutor &databaseExecutor_;
};

} // namespace webserver::phase11::transport
