#pragma once
#include "backend/application/modules/admin/application/AdminService/AdminService.h"
#include "backend/application/modules/tree/application/TreeService/TreeService.h"
#include "backend/application/modules/moderation/application/ModerationService/ModerationService.h"
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
namespace webserver::function3 {
class AdminHttp final {
public:
    AdminHttp(phase11::transport::Phase11AuthHttp&auth,phase11::DatabaseExecutor&executor,AdminService&admin,ModerationService&moderation,TreeService&tree,std::function<void(Id)>revoked={}):auth_(auth),executor_(executor),admin_(admin),moderation_(moderation),tree_(tree),revoked_(std::move(revoked)){}
    void registerRoutes(Router &router);
private:phase11::transport::Phase11AuthHttp&auth_;phase11::DatabaseExecutor&executor_;AdminService&admin_;ModerationService&moderation_;TreeService&tree_;std::function<void(Id)>revoked_;
};
}
