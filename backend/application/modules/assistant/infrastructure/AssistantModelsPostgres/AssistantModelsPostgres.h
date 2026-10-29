#pragma once
#include "backend/application/modules/assistant/ports/IAssistantModels/IAssistantModels.h"
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
namespace webserver::function3 {
class AssistantModelsPostgres final:public IAssistantModels{
public:explicit AssistantModelsPostgres(SqlContext&sql):sql_(sql){}
    std::vector<AssistantModelEntry>configured()const override;
private:SqlContext&sql_;
};
}
