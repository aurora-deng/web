#pragma once
#include "backend/application/transport/http/BusinessHttpExecutor/BusinessHttpExecutor.h"
#include "backend/application/business/BusinessInterfaces/BusinessInterfaces.h"
class Router;
class WebSocketSessionManager;
namespace webserver::phase11::transport {
/** GroupGovernance endpoints depend only on their domain business port. */
class GroupGovernanceHttp final:private BusinessHttpExecutor {
public:
    GroupGovernanceHttp(Phase11AuthHttp &auth,IChatBusiness &business,DatabaseExecutor &executor,std::function<void()> committed={}):BusinessHttpExecutor(auth,executor,std::move(committed)),chat_(business){}
    void registerRoutes(Router &router);
private:
    IChatBusiness &chat_;
};
}
