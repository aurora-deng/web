#include "AdminService.h"
#include "backend/application/modules/shared/TextRules/TextRules.h"
namespace webserver::function3 {
void AdminService::administrator(Id actor) const {if(activeActor(actors_,actor).role!=Role::Administrator)throw BusinessError(Failure::Forbidden,"site administrator required");}
std::vector<SiteUser> AdminService::users(Id actor,Id after) const {administrator(actor);return repository_.users(after,50);}
std::vector<AuditRecord> AdminService::audits(Id actor,Id before) const {moderator(activeActor(actors_,actor));return repository_.audits(before,50);}
namespace {void reasonRequired(std::string_view reason){if(trimText(std::string(reason)).empty()||!textLength(reason,200))throw BusinessError(Failure::Invalid,"audit reason required");}}
void AdminService::role(Id actor,Id user,Role role,std::string reason) {
    reasonRequired(reason);
    work_.transact([&]{repository_.lockRoles();administrator(actor);auto target=actors_.requireActor(user);
        if(!target.interactive&&role!=Role::User)throw BusinessError(Failure::Invalid,"AI/service accounts cannot hold site management roles");
        if(target.active && target.interactive && target.role==Role::Administrator && role!=Role::Administrator && repository_.activeAdministrators()<=1)throw BusinessError(Failure::Conflict,"last active administrator must remain");
        repository_.setRole(user,role);repository_.revokeSessions(user);repository_.audit(actor,"role",user,reason);
    });
}
void AdminService::active(Id actor,Id user,bool value,std::string reason) {
    reasonRequired(reason);
    work_.transact([&]{repository_.lockRoles();administrator(actor);auto target=actors_.requireActor(user);
        if(!value && target.active && target.interactive && target.role==Role::Administrator && repository_.activeAdministrators()<=1)throw BusinessError(Failure::Conflict,"last active administrator must remain");
        repository_.setActive(user,value);if(!value)repository_.revokeSessions(user);repository_.audit(actor,value?"enable":"disable",user,reason);
    });
}
void AdminService::revoke(Id actor,Id user,std::string reason) {
    reasonRequired(reason);work_.transact([&]{administrator(actor);actors_.requireActor(user);repository_.revokeSessions(user);repository_.audit(actor,"revoke_sessions",user,reason);});
}
}
