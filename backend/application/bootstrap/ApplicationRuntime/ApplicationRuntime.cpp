#include "ApplicationRuntime.h"
#include "backend/application/bootstrap/BootstrapSettings/BootstrapSettings.h"
#include "backend/application/bootstrap/NetworkSetup/NetworkSetup.h"
#include "backend/application/bootstrap/BusinessRuntime/BusinessRuntime.h"
#include "backend/application/bootstrap/OperationalRuntime/OperationalRuntime.h"
#include "backend/application/bootstrap/GrpcRuntime/GrpcRuntime.h"
#include "backend/application/modules/shared/transport/FrontendAssets/FrontendAssets.h"
#include "backend/application/transport/learning/LearningRoutes/LearningRoutes.h"
#include "backend/server/Runtime/ServerRuntime.h"
namespace webserver::bootstrap {
int ApplicationRuntime::run(){
    const auto config=config::RuntimeConfig::load();
    ServerRuntime server({size("WEB_HTTP_WORKERS",0,1,32),size("WEB_WS_WORKERS",0,1,32)});
    configureNetwork(server,config.production);
    OperationalRuntime operations(config.production);
    // ServerRuntime::start drains network tasks before returning/rethrowing.
    // Before start fails at listen/TLS setup, no Reactor has accepted work.
    // BusinessRuntime stops before the still-live server directories disappear.
    BusinessRuntime business(server,config.production);
    operations.install(server,business.authentication(),[&]{return business.ready();},[&]{return business.metrics();},business.enabled());
    business.registerRoutes(server.router());
    function3::registerFrontendAssets(server.router());
    learning::registerLearningRoutes(server);
    GrpcRuntime grpc(config.production,operations.authSecret(),operations.metrics());
    server.start();
    grpc.stop();
    business.stop();
    if(!business.stoppedCleanly())throw std::runtime_error("business shutdown reported failures");
    return 0;
}
}
