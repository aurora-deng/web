#pragma once
#include "backend/application/modules/announcement/ports/IAnnouncementRepository/IAnnouncementRepository.h"
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
namespace webserver::function3 {
class AnnouncementPostgresRepository final:public IAnnouncementRepository {
public:
    explicit AnnouncementPostgresRepository(SqlContext &sql):sql_(sql){}
    Announcement create(Id version,std::string text) override;
    std::vector<Announcement> recent(Id after,std::size_t limit) const override;
    NotificationSettings settings(Id user) const override;
    void settings(Id user,const NotificationSettings &value) override;
    bool visible(Id announcement)const override;
    bool claimPlayback(Id user,Id announcement)override;
private:SqlContext &sql_;
};
}
