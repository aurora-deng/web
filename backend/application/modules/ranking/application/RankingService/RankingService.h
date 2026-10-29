#pragma once
#include "backend/application/modules/ranking/ports/IRankingRepository/IRankingRepository.h"
namespace webserver::function3 {
class RankingService final {
public:
    RankingService(IUnitOfWork &work,IRankingRepository &repository,ITreeRankingSource &tree,IRankAnnouncement &announcements)
        :work_(work),repository_(repository),tree_(tree),announcements_(announcements){}
    bool settle();
    RankSnapshot latest() const {return repository_.latest();}
private:
    IUnitOfWork &work_;IRankingRepository &repository_;ITreeRankingSource &tree_;IRankAnnouncement &announcements_;
};
}
