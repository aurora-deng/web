#pragma once
#include "backend/ai/private/application/PrivateGenerationService/PrivateGenerationService.h"
#include "backend/application/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"
namespace webserver::phase11::transport {
class PrivateGenerationHttp final {
public:
    PrivateGenerationHttp(Phase11AuthHttp&auth,DatabaseExecutor&executor,PrivateGenerationService&service,bool web=false)
        :auth_(auth),executor_(executor),service_(service),web_(web){}
    void registerRoutes(Router&router);
private:Phase11AuthHttp&auth_;DatabaseExecutor&executor_;PrivateGenerationService&service_;bool web_{};
};
}
