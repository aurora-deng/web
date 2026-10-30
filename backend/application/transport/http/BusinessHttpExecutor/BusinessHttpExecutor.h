#pragma once
#include "backend/application/business/ApplicationError/ApplicationError.h"
#include "backend/application/runtime/BoundedExecutor/BoundedExecutor.h"
#include "backend/application/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"
#include <future>
#include <functional>
#include <type_traits>
namespace webserver::phase11::transport {
/** Shared execution/commit notification only. Each HTTP adapter owns its own
 * business port. Routes are registered on the database executor lane. */
class BusinessHttpExecutor {
protected:
    BusinessHttpExecutor(Phase11AuthHttp &auth,DatabaseExecutor &executor,std::function<void()> committed)
        :authentication_(auth),databaseExecutor_(executor),outboxCommitted_(std::move(committed)){}
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
                                   "database service is busy");
        return result.get();
    }

    /** 写操作已提交后再叫醒 Outbox；失败事务不会发出虚假通知。 */
    template<class Operation>
    auto runMutation(Operation &&operation)
        -> std::invoke_result_t<std::decay_t<Operation>>
    {
        using Result = std::invoke_result_t<std::decay_t<Operation>>;
        if constexpr (std::is_void_v<Result>)
        {
            runDatabase(std::forward<Operation>(operation));
            if (outboxCommitted_) outboxCommitted_();
        }
        else
        {
            auto result = runDatabase(std::forward<Operation>(operation));
            if (outboxCommitted_) outboxCommitted_();
            return result;
        }
    }


    Phase11AuthHttp &authentication_;
    DatabaseExecutor &databaseExecutor_;
    std::function<void()>outboxCommitted_;
};
}
