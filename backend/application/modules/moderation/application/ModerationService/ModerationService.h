#pragma once
#include "backend/application/modules/moderation/ports/IReportRepository/IReportRepository.h"
#include "backend/application/modules/tree/ports/ITreeRepository/ITreeRepository.h"
namespace webserver::function3 {
class ModerationService final {
public:
    ModerationService(IUnitOfWork&work,IActorDirectory&actors,ITreeRepository&tree,IReportRepository&reports,IEventWriter&events):work_(work),actors_(actors),tree_(tree),reports_(reports),events_(events){}
    ContentReport report(Id actor,Id post,std::string reason);
    std::vector<ContentReport> pending(Id actor,Id before)const;
    void resolve(Id actor,Id report,bool dismiss,std::string reason,Milliseconds now);
private:IUnitOfWork&work_;IActorDirectory&actors_;ITreeRepository&tree_;IReportRepository&reports_;IEventWriter&events_;
};
}
