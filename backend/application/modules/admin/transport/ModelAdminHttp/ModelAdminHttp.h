#pragma once
#include "backend/application/modules/admin/application/ModelAdminService/ModelAdminService.h"
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
#include <memory>
#include <mutex>
namespace webserver::function3 {
class ModelAdminHttp final {
public:
    ModelAdminHttp(phase11::transport::Phase11AuthHttp&auth,phase11::BoundedExecutor&executor,ModelAdminService&service):auth_(auth),executor_(executor),service_(service){}
    void registerRoutes(Router&router);
private:
    struct Operation {Id actor{},node{},version{};bool rollback{};Milliseconds updated{};std::string state="pending";};
    struct State {std::mutex mutex;std::map<Id,Operation>operations;Id next=static_cast<Id>(http::now());};
    std::shared_ptr<State> state_=std::make_shared<State>();
    Id enqueue(Id actor,Id node,Id version,bool rollback,std::string reason);
    phase11::transport::Phase11AuthHttp&auth_;phase11::BoundedExecutor&executor_;ModelAdminService&service_;
};
}
