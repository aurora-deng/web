#pragma once
#include "backend/application/modules/tree/application/TreeMediaService/TreeMediaService.h"
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
namespace webserver::function3 {
class TreeMediaHttp final {
public:
    TreeMediaHttp(phase11::transport::Phase11AuthHttp&auth,phase11::BoundedExecutor&executor,TreeMediaService&media):auth_(auth),executor_(executor),media_(media){}
    void registerRoutes(Router&router);
private:phase11::transport::Phase11AuthHttp&auth_;phase11::BoundedExecutor&executor_;TreeMediaService&media_;
};
}
