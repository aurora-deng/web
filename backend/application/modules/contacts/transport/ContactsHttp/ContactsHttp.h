#pragma once
#include "backend/application/transport/http/BusinessHttpExecutor/BusinessHttpExecutor.h"
#include "backend/application/business/BusinessInterfaces/BusinessInterfaces.h"
class Router;
class WebSocketSessionManager;
namespace webserver::phase11::transport {
/** Contacts endpoints depend only on their domain business port. */
class ContactsHttp final:private BusinessHttpExecutor {
public:
    ContactsHttp(Phase11AuthHttp &auth,ISocialBusiness &business,DatabaseExecutor &executor,WebSocketSessionManager &sockets,std::function<void()> committed={}):BusinessHttpExecutor(auth,executor,std::move(committed)),social_(business),webSockets_(sockets){}
    void registerRoutes(Router &router);
private:
    ISocialBusiness &social_;
    WebSocketSessionManager &webSockets_;
};
}
