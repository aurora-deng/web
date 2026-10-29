#pragma once
#include "backend/application/modules/admin/ports/IModelAdministration/IModelAdministration.h"
namespace webserver::function3 {
class ModelAdminService final {
public:
    ModelAdminService(IActorDirectory&actors,IModelAdministration&models,IUnitOfWork&work,IEventWriter&events):actors_(actors),models_(models),work_(work),events_(events){}
    std::vector<ManagedModel>nodes(Id actor,Id after)const;
    std::vector<ManagedVersion>versions(Id actor,Id node,Id after)const;
    QualityDecision assess(Id actor,Id node,Id version)const;
    void change(Id actor,Id node,Id version,bool rollback,std::string reason);
    void requireAdministrator(Id actor)const{authorize(actor);}
private:
    void authorize(Id actor)const;
    IActorDirectory&actors_;IModelAdministration&models_;
    IUnitOfWork&work_;IEventWriter&events_;
};
}
