#include "ModelAdminService.h"
#include "backend/application/modules/shared/TextRules/TextRules.h"
#include <chrono>
namespace webserver::function3 {
void ModelAdminService::authorize(Id actor)const{if(activeActor(actors_,actor).role!=Role::Administrator)throw BusinessError(Failure::Forbidden,"site administrator required");}
std::vector<ManagedModel>ModelAdminService::nodes(Id actor,Id after)const{authorize(actor);return models_.nodes(after);}
std::vector<ManagedVersion>ModelAdminService::versions(Id actor,Id node,Id after)const{authorize(actor);return models_.versions(node,after);}
QualityDecision ModelAdminService::assess(Id actor,Id node,Id version)const{authorize(actor);return models_.assess(node,version);}
void ModelAdminService::change(Id actor,Id node,Id version,bool rollback,std::string reason){
    authorize(actor);reason=trimText(std::move(reason));if(!textLength(reason,200))throw BusinessError(Failure::Invalid,"model change reason required (max 200 characters)");
    // 原控制服务把校验结果、操作者与版本切换一起审计；不将前端填写的“通过”视为评测证据。
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    work_.transact([&]{events_.appendEvent({0,"admin.audit","model.operation",node,static_cast<std::uint64_t>(now),{{"actorId",std::to_string(actor)},{"action",rollback?"model.rollback.requested":"model.activate.requested"},{"reason","version="+std::to_string(version)+"; "+reason}},now});});
    models_.change(node,version,rollback,"site-user:"+std::to_string(actor),[this,actor]{authorize(actor);});
}
}
