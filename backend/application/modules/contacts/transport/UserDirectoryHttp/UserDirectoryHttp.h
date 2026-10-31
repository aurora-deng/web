#pragma once
#include "backend/application/modules/contacts/application/UserDirectoryService/UserDirectoryService.h"
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
namespace webserver::contacts {
class UserDirectoryHttp final {
public:
    UserDirectoryHttp(phase11::transport::Phase11AuthHttp&auth,phase11::DatabaseExecutor&executor,UserDirectoryService&service):auth_(auth),executor_(executor),service_(service){}
    void registerRoutes(Router&);
private:
    phase11::transport::Phase11AuthHttp&auth_;
    phase11::DatabaseExecutor&executor_;
    UserDirectoryService&service_;
};
}
