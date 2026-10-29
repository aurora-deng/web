#pragma once
#include "backend/application/modules/messaging/application/MessagingService/MessagingService.h"
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
namespace webserver::function3 {
class MessagingHttp final {
public:
    MessagingHttp(phase11::transport::Phase11AuthHttp&auth,phase11::DatabaseExecutor&executor,MessagingService&service):auth_(auth),executor_(executor),service_(service){}
    void registerRoutes(Router&router);
private:phase11::transport::Phase11AuthHttp&auth_;phase11::DatabaseExecutor&executor_;MessagingService&service_;
};
}
