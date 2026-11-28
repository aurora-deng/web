#pragma once
#include "backend/application/modules/shared/transport/BusinessHttpExecutor/BusinessHttpExecutor.h"
#include "backend/application/modules/chat/ports/IChatBusiness/IChatBusiness.h"
class Router;
class WebSocketSessionManager;
namespace webserver::phase11::transport {
/** Conversations endpoints depend only on their domain business port. */
class ConversationsHttp final:private BusinessHttpExecutor {
public:
    ConversationsHttp(Phase11AuthHttp &auth,IChatBusiness &business,DatabaseExecutor &executor,std::function<void()> committed={}):BusinessHttpExecutor(auth,executor,std::move(committed)),chat_(business){}
    void registerRoutes(Router &router);
private:
    IChatBusiness &chat_;
};
}
