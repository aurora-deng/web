#include "ModerationService.h"
#include "backend/application/modules/shared/TextRules/TextRules.h"
namespace webserver::function3 {
ContentReport ModerationService::report(Id actor,Id post,std::string reason){
    reason=trimText(std::move(reason));
    if(!textLength(reason,200))throw BusinessError(Failure::Invalid,"report reason required");
    ContentReport result;work_.transact([&]{activeActor(actors_,actor);auto p=tree_.post(post,actor);if(!p||p->state!=PostState::Published)throw BusinessError(Failure::NotFound,"fruit not found");result=reports_.report(post,actor,reason);});return result;
}
std::vector<ContentReport> ModerationService::pending(Id actor,Id before)const{moderator(activeActor(actors_,actor));return reports_.pending(before,50);}
void ModerationService::resolve(Id actor,Id report,bool dismiss,std::string reason,Milliseconds now){
    reason=trimText(std::move(reason));
    if(!textLength(reason,200))throw BusinessError(Failure::Invalid,"decision reason required");
    work_.transact([&]{moderator(activeActor(actors_,actor));reports_.resolve(report,dismiss);events_.appendEvent({0,"admin.audit","report",report,static_cast<std::uint64_t>(now),{{"actorId",std::to_string(actor)},{"action",dismiss?"dismiss_report":"resolve_report"},{"reason",reason}},now});});
}
}
