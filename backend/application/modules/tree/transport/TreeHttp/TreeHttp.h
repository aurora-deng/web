#pragma once
#include "backend/application/modules/tree/application/TreeService/TreeService.h"
#include "backend/application/modules/moderation/application/ModerationService/ModerationService.h"
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
#include "backend/application/modules/cache/ports/IQueryCache/IQueryCache.h"
namespace webserver::function3 {
class TreeHttp final {
public:
    TreeHttp(phase11::transport::Phase11AuthHttp&auth,phase11::DatabaseExecutor&executor,TreeService&tree,ModerationService&moderation):auth_(auth),executor_(executor),tree_(tree),moderation_(moderation){}
    void registerRoutes(Router &router);
    void setCache(IQueryCache*cache){cache_=cache;}
private:phase11::transport::Phase11AuthHttp&auth_;phase11::DatabaseExecutor&executor_;TreeService&tree_;ModerationService&moderation_;IQueryCache*cache_{};
};
}
