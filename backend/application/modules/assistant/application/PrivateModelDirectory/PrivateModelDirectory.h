#pragma once
#include "backend/application/modules/assistant/ports/IAssistantModels/IAssistantModels.h"
namespace webserver::function3 {
class PrivateModelDirectory final{
public:explicit PrivateModelDirectory(IAssistantModels&models):models_(models){}
    std::vector<AssistantModelEntry>list()const;
private:IAssistantModels&models_;
};
}
