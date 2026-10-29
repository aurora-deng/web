#pragma once
#include "backend/application/modules/activities/application/ActivityService/ActivityService.h"
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
#include "backend/application/business/BusinessInterfaces/BusinessInterfaces.h"
namespace webserver::function3 {
class ActivityHttp final {
public:
    ActivityHttp(phase11::transport::Phase11AuthHttp&auth,phase11::BoundedExecutor&executor,ActivityService&service,phase11::ISocialBusiness&social)
        :auth_(auth),executor_(executor),service_(service),social_(social){}
    void registerRoutes(Router&router);
private:phase11::transport::Phase11AuthHttp&auth_;phase11::BoundedExecutor&executor_;ActivityService&service_;phase11::ISocialBusiness&social_;
};
}
