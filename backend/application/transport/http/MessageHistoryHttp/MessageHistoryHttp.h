#pragma once
#include "backend/application/transport/http/BusinessHttpExecutor/BusinessHttpExecutor.h"
#include "backend/application/business/BusinessInterfaces/BusinessInterfaces.h"
#include "backend/application/runtime/UserRecoveryBudget/UserRecoveryBudget.h"
class Router;
class WebSocketSessionManager;
namespace webserver::phase11::transport {
/** MessageHistory endpoints depend only on their domain business port. */
class MessageHistoryHttp final:private BusinessHttpExecutor {
public:
    MessageHistoryHttp(Phase11AuthHttp &auth,IChatBusiness &business,DatabaseExecutor &executor,std::function<void()> committed={}):BusinessHttpExecutor(auth,executor,std::move(committed)),chat_(business){}
    void registerRoutes(Router &router);
private:
    IChatBusiness &chat_;
    UserRecoveryBudget historyBudget_;
};
}
