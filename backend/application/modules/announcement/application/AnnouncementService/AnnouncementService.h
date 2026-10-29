#pragma once
#include "backend/application/modules/announcement/ports/IAnnouncementRepository/IAnnouncementRepository.h"
#include "backend/application/modules/ranking/ports/IRankingRepository/IRankingRepository.h"
namespace webserver::function3 {
class AnnouncementService final:public IRankAnnouncement {
public:
    AnnouncementService(IUnitOfWork &work,IActorDirectory &actors,IAnnouncementRepository &repository,IEventWriter &events)
        :work_(work),actors_(actors),repository_(repository),events_(events){}
    void rankingChanged(const RankSnapshot &,const std::vector<RankCandidate> &) override;
    std::vector<Announcement> recent(Id actor,Id after) const;
    NotificationSettings settings(Id actor) const;
    void settings(Id actor,NotificationSettings value);
    void requireVisible(Id actor,Id announcement)const;
    bool claimPlayback(Id actor,Id announcement);
private:
    IUnitOfWork &work_;IActorDirectory &actors_;IAnnouncementRepository &repository_;IEventWriter &events_;
};
}
