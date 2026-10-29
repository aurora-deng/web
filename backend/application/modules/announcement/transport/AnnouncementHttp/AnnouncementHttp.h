#pragma once
#include "backend/application/modules/announcement/application/AnnouncementService/AnnouncementService.h"
#include "backend/application/modules/ranking/application/RankingService/RankingService.h"
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
#include "backend/application/modules/cache/ports/IQueryCache/IQueryCache.h"
namespace webserver::function3 {
class AnnouncementHttp final {
public:
    AnnouncementHttp(phase11::transport::Phase11AuthHttp&auth,phase11::DatabaseExecutor&executor,AnnouncementService&announcements,RankingService&rank):auth_(auth),executor_(executor),announcements_(announcements),rank_(rank){}
    void registerRoutes(Router &router);
    void setCache(IQueryCache*cache){cache_=cache;}
private:phase11::transport::Phase11AuthHttp&auth_;phase11::DatabaseExecutor&executor_;AnnouncementService&announcements_;RankingService&rank_;IQueryCache*cache_{};
};
}
