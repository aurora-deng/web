#pragma once
#include "backend/application/modules/admin/ports/IAdminRepository/IAdminRepository.h"
namespace webserver::function3 {
class AdminService final {
public:
    AdminService(IUnitOfWork &work,IActorDirectory &actors,IAdminRepository &repository):work_(work),actors_(actors),repository_(repository){}
    Actor identity(Id actor) const {return activeActor(actors_,actor);}
    std::vector<SiteUser> users(Id actor,Id after) const;
    std::vector<AuditRecord> audits(Id actor,Id before) const;
    void role(Id actor,Id user,Role role,std::string reason);
    void active(Id actor,Id user,bool value,std::string reason);
    void revoke(Id actor,Id user,std::string reason);
private:
    void administrator(Id actor) const;
    IUnitOfWork &work_;IActorDirectory &actors_;IAdminRepository &repository_;
};
}
