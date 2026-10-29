#pragma once
#include "backend/application/modules/assistant/ports/IPrivateArchive/IPrivateArchive.h"
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
#include "backend/application/modules/messaging/infrastructure/ChatOutboxWriter/ChatOutboxWriter.h"
namespace webserver::function3 {
/** 旧数据迁移的防腐适配器；只有这里知道旧知识/聊天表，不进入私人推理路径。 */
class LegacyArchivePostgres final:public IPrivateArchive{
public:
    LegacyArchivePostgres(SqlContext&sql,std::function<void()>committed):sql_(sql),events_(std::move(committed)){}
    std::vector<LegacyPrivateResource>resources(Id owner)override;
    LegacyPrivatePage exportPage(Id owner,std::string kind,Id resource,Id after,Id upper)override;
    void purgePage(Id owner,std::string kind,Id resource,Id after,Id through,std::string checksum,const std::function<std::string(const LegacyPrivatePage&)>&digest)override;
private:LegacyPrivatePage read(Id owner,std::string kind,Id resource,Id after,Id upper,bool exact);SqlContext&sql_;ChatOutboxWriter events_;
};
}
