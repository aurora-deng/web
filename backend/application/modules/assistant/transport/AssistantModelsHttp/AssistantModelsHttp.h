#pragma once
#include "backend/application/modules/assistant/application/PrivateModelDirectory/PrivateModelDirectory.h"
#include "backend/application/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"
namespace webserver::function3 {
class AssistantModelsHttp final{
public:AssistantModelsHttp(phase11::transport::Phase11AuthHttp&auth,phase11::DatabaseExecutor&executor,PrivateModelDirectory&models):auth_(auth),executor_(executor),models_(models){}
    void registerRoutes(Router&router);
private:phase11::transport::Phase11AuthHttp&auth_;phase11::DatabaseExecutor&executor_;PrivateModelDirectory&models_;
};
}
