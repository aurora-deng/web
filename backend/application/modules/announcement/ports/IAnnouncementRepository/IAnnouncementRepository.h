#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
#include <vector>
namespace webserver::function3 {
struct Announcement { Id id{},version{}; std::string text; Milliseconds createdAt{},expiresAt{}; };
struct NotificationSettings {bool sound{};std::string voice{"zf_001"};};
class IAnnouncementRepository {
public:
    virtual ~IAnnouncementRepository()=default;
    virtual Announcement create(Id rankVersion,std::string text)=0;
    virtual std::vector<Announcement> recent(Id after,std::size_t limit) const=0;
    virtual NotificationSettings settings(Id user) const=0;
    virtual void settings(Id user,const NotificationSettings &value)=0;
    virtual bool visible(Id announcement)const=0;
    virtual bool claimPlayback(Id user,Id announcement)=0;
};
}
