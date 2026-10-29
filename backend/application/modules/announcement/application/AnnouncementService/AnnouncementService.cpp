#include "AnnouncementService.h"
namespace webserver::function3 {
void AnnouncementService::rankingChanged(const RankSnapshot &snapshot,const std::vector<RankCandidate> &candidates) {
    std::string text="神树七日热榜已更新。";
    if(candidates.empty()) text+="本轮暂无上榜果实。";
    for(std::size_t i=0;i<candidates.size() && i<3;++i)
        text+="第"+std::to_string(i+1)+"名，"+candidates[i].authorName+"的果实。";
    auto value=repository_.create(snapshot.version,std::move(text));
    events_.appendEvent({0,"announcement.created","announcement",value.id,snapshot.version,
        {{"announcementId",std::to_string(value.id)},{"text",value.text}},snapshot.settledAt});
    events_.appendEvent({0,"speech.requested","announcement",value.id,snapshot.version,
        {{"announcementId",std::to_string(value.id)},{"text",value.text},{"expiresAt",std::to_string(value.expiresAt)}},snapshot.settledAt});
}
std::vector<Announcement> AnnouncementService::recent(Id actor,Id after) const {activeActor(actors_,actor);return repository_.recent(after,20);}
NotificationSettings AnnouncementService::settings(Id actor) const {activeActor(actors_,actor);return repository_.settings(actor);}
void AnnouncementService::requireVisible(Id actor,Id announcement)const{activeActor(actors_,actor);if(!repository_.visible(announcement))throw BusinessError(Failure::NotFound,"announcement expired or unavailable");}
bool AnnouncementService::claimPlayback(Id actor,Id announcement){bool result=false;work_.transact([&]{requireVisible(actor,announcement);result=repository_.claimPlayback(actor,announcement);});return result;}
void AnnouncementService::settings(Id actor,NotificationSettings value) {
    if(value.voice!="zf_001" && value.voice!="zm_009")throw BusinessError(Failure::Invalid,"unsupported announcement voice");
    work_.transact([&]{activeActor(actors_,actor);repository_.settings(actor,value);});
}
}
