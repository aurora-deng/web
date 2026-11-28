#pragma once
#include "backend/application/modules/accounts/ports/IUserRepository/IUserRepository.h"
#include "backend/ai/private/ports/IPrivateInference/IPrivateInference.h"
#include "backend/ai/registry/ModelRegistryService/ModelRegistryService.h"
namespace webserver::phase11 {
class PrivateModelRouter final:public IPrivateInference {
public:
    PrivateModelRouter(ModelRouter&models,IUserRepository&users):models_(models),users_(users){}
    GenerationHandle generate(UserId ai,GenerationRequest request,TokenSink token,
        CompletionSink complete,CancellationToken cancellation)override;
private:ModelRouter&models_;IUserRepository&users_;
};
}
