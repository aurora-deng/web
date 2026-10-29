#include "AnnouncementPostgresRepository.h"
namespace webserver::function3 {
bool AnnouncementPostgresRepository::visible(Id announcement)const{return !sql_.query("SELECT 1 FROM announcements WHERE id=$1 AND expires_at>CURRENT_TIMESTAMP",{std::to_string(announcement)}).empty();}
bool AnnouncementPostgresRepository::claimPlayback(Id user,Id announcement){sql_.requireTransaction();return !sql_.query("INSERT INTO announcement_playback_claims(user_id,announcement_id) SELECT $1,a.id FROM announcements a JOIN notification_settings n ON n.user_id=$1 AND n.announcement_sound WHERE a.id=$2 AND a.expires_at>CURRENT_TIMESTAMP ON CONFLICT DO NOTHING RETURNING announcement_id",{std::to_string(user),std::to_string(announcement)}).empty();}
namespace {
Announcement parse(const std::vector<std::string> &r){return {std::stoull(r[0]),std::stoull(r[1]),r[2],std::stoll(r[3]),std::stoll(r[4])};}
const std::string columns="id,rank_version,text,floor(extract(epoch FROM created_at)*1000)::bigint,floor(extract(epoch FROM expires_at)*1000)::bigint";
}
Announcement AnnouncementPostgresRepository::create(Id version,std::string text) {
    sql_.requireTransaction();
    auto rows=sql_.query("INSERT INTO announcements(rank_version,text) VALUES($1,$2) RETURNING "+columns,{std::to_string(version),std::move(text)});
    const auto value=parse(rows[0]);
    return value;
}
std::vector<Announcement> AnnouncementPostgresRepository::recent(Id after,std::size_t limit) const {
    std::vector<Announcement> result;
    // 只补发未过期公告；已有历史接口可扩展，不把断线期间的旧公告突然读出来。
    for(auto &r:sql_.query("SELECT "+columns+" FROM announcements WHERE id>$1 AND expires_at>CURRENT_TIMESTAMP ORDER BY id LIMIT $2",{std::to_string(after),std::to_string(limit)}))result.push_back(parse(r));
    return result;
}
NotificationSettings AnnouncementPostgresRepository::settings(Id user) const {
    auto r=sql_.query("SELECT announcement_sound,voice FROM notification_settings WHERE user_id=$1",{std::to_string(user)});
    return r.empty()?NotificationSettings{}:NotificationSettings{r[0][0]=="t",r[0][1]};
}
void AnnouncementPostgresRepository::settings(Id user,const NotificationSettings &value) {
    sql_.requireTransaction();sql_.query("INSERT INTO notification_settings VALUES($1,$2,$3) ON CONFLICT(user_id) DO UPDATE SET announcement_sound=EXCLUDED.announcement_sound,voice=EXCLUDED.voice",{std::to_string(user),value.sound?"true":"false",value.voice});
}
}
