#pragma once
#include "backend/application/modules/assistant/application/PrivateArchiveService/PrivateArchiveService.h"
#include "backend/application/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"
namespace webserver::function3 {
class PrivateArchiveHttp final{
public:PrivateArchiveHttp(phase11::transport::Phase11AuthHttp&auth,phase11::DatabaseExecutor&executor,PrivateArchiveService&service):auth_(auth),executor_(executor),service_(service){}
    void registerRoutes(Router&router);
private:phase11::transport::Phase11AuthHttp&auth_;phase11::DatabaseExecutor&executor_;PrivateArchiveService&service_;
};
}
