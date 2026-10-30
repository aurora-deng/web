#pragma once
#include "backend/application/ports/IModelRepository/IModelRepository.h"
#include "backend/application/modules/admin/ports/IModelAdministration/IModelAdministration.h"
#include "backend/ai/models/ModelRegistryService/ModelRegistryService.h"
#include "backend/ai/models/ModelEvaluationService/ModelEvaluationService.h"
namespace webserver::function3 {
class Phase11ModelAdministration final:public IModelAdministration {
public:
    Phase11ModelAdministration(phase11::IModelRepository&models,phase11::ModelRegistryService&registry,phase11::ModelEvaluationService&evaluation):models_(models),registry_(registry),evaluation_(evaluation){}
    std::vector<ManagedModel>nodes(Id after)const override;
    std::vector<ManagedVersion>versions(Id node,Id after)const override;
    QualityDecision assess(Id node,Id version)const override;
    void change(Id node,Id version,bool rollback,std::string actor,std::function<void()>beforeCommit)override;
private:
    phase11::IModelRepository&models_;phase11::ModelRegistryService&registry_;phase11::ModelEvaluationService&evaluation_;
};
}
