#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
#include <vector>
namespace webserver::function3 {
struct SiteUser {Actor actor;std::string username;};
struct AuditRecord {Id id{},actor{};std::string action,resource,reason,at;};
class IAdminRepository {
public:
    virtual ~IAdminRepository()=default;
    virtual void lockRoles()=0;
    virtual std::vector<SiteUser> users(Id after,std::size_t limit) const=0;
    virtual std::size_t activeAdministrators() const=0;
    virtual void setRole(Id user,Role role)=0;
    virtual void setActive(Id user,bool active)=0;
    virtual void revokeSessions(Id user)=0;
    virtual void audit(Id actor,std::string action,Id user,std::string reason)=0;
    virtual std::vector<AuditRecord> audits(Id before,std::size_t limit) const=0;
};
}
