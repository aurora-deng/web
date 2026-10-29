#include "AssistantModelsHttp.h"
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
namespace webserver::function3 {
void AssistantModelsHttp::registerRoutes(Router&router){
    auto execution=http::executor(router,executor_);
    for(const auto&path:{"/api/ai/private/models","/api/ai/web/models"})router.GET(path,[this,privateMode=std::string(path).find("/private/")!=std::string::npos](RequestContext&c){return http::guarded(c,[&]{
        if(privateMode&&!c.transportSecure)throw BusinessError(Failure::Forbidden,"private model directory requires HTTPS");
        auth_.authorizeRequest(c);http::reply(c,http::array(models_.list(),[](const auto&m){return "{\"accountId\":"+http::id(m.account)+",\"nodeId\":"+http::id(m.node)+",\"versionId\":"+http::id(m.version)+",\"name\":"+http::quote(m.name)+",\"runtime\":"+http::quote(m.runtime)+",\"status\":\"configured\",\"automaticTools\":false}";}));
    });});
}
}
