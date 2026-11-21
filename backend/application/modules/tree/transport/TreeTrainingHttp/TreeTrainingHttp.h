#pragma once
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
#include "backend/application/modules/tree/application/TreeTrainingService/TreeTrainingService.h"

namespace webserver::function3 {
class TreeTrainingHttp final {
public:
    TreeTrainingHttp(phase11::transport::Phase11AuthHttp& auth, phase11::DatabaseExecutor& executor,
                     TreeTrainingService& service)
        : auth_(auth), executor_(executor), service_(service) {}
    void registerRoutes(Router& router);
private:
    phase11::transport::Phase11AuthHttp& auth_;
    phase11::DatabaseExecutor& executor_;
    TreeTrainingService& service_;
};
}
