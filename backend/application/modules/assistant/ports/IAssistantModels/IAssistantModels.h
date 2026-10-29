#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
#include <vector>
namespace webserver::function3 {
struct AssistantModelEntry{Id account{},node{},version{};std::string name,runtime;};
class IAssistantModels{public:virtual~IAssistantModels()=default;virtual std::vector<AssistantModelEntry>configured()const=0;};
}
