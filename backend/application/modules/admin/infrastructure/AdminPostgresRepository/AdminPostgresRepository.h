#pragma once
#include "backend/application/modules/admin/ports/IAdminRepository/IAdminRepository.h"
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
namespace webserver::function3 {
class AdminPostgresRepository final:public IAdminRepository {
public:
    explicit AdminPostgresRepository(SqlContext &sql):sql_(sql){}
    void lockRoles() override;
    std::vector<SiteUser> users(Id after,std::size_t limit) const override;
    std::size_t activeAdministrators() const override;
    void setRole(Id user,Role role) override;
    void setActive(Id user,bool active) override;
    void revokeSessions(Id user) override;
    void audit(Id actor,std::string action,Id user,std::string reason) override;
    std::vector<AuditRecord> audits(Id before,std::size_t limit) const override;
private:SqlContext &sql_;
};
}
