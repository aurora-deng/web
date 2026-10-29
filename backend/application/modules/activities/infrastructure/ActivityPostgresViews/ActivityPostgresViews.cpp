#include "ActivityPostgresViews.h"
namespace webserver::function3 {
void ActivityPostgresViews::set(Id user,Id post,bool enabled,Milliseconds now) {
    sql_.requireTransaction();
    if(enabled)sql_.query("INSERT INTO activity_favorites VALUES($1,$2,$3) ON CONFLICT DO NOTHING",{std::to_string(user),std::to_string(post),std::to_string(now)});
    else sql_.query("DELETE FROM activity_favorites WHERE user_id=$1 AND post_id=$2",{std::to_string(user),std::to_string(post)});
}
std::set<Id> ActivityPostgresViews::contained(Id user,const std::vector<Id>&posts)const {
    if(posts.empty())return{};
    if(posts.size()>100)throw BusinessError(Failure::Invalid,"bookmark batch too large");
    std::string values="{";for(Id id:posts){if(values.size()>1)values+=",";values+=std::to_string(id);}values+="}";
    std::set<Id> result;
    for(const auto&r:sql_.query("SELECT post_id FROM activity_favorites WHERE user_id=$1 AND post_id=ANY($2::bigint[])",{std::to_string(user),values}))result.insert(std::stoull(r[0]));
    return result;
}
std::vector<Id> ActivityPostgresViews::page(Id user,Id before,std::size_t limit)const {
    std::vector<Id>result;
    for(const auto&r:sql_.query("SELECT post_id FROM activity_favorites WHERE user_id=$1 AND ($2::bigint=0 OR post_id<$2) ORDER BY post_id DESC LIMIT $3",{std::to_string(user),std::to_string(before),std::to_string(limit)}))result.push_back(std::stoull(r[0]));return result;
}
PublicProfile ActivityPostgresViews::profile(Id viewer,Id user)const {
    auto rows=sql_.query("SELECT u.username,u.display_name,u.biography,u.avatar_file_id,u.ai_account,COALESCE(s.show_biography,true),COALESCE(s.show_avatar,true),COALESCE(s.allow_tree_friend_requests,false),"
        "EXISTS(SELECT 1 FROM friendships f WHERE f.user_low_id=LEAST($1::bigint,$2::bigint) AND f.user_high_id=GREATEST($1::bigint,$2::bigint)),"
        "EXISTS(SELECT 1 FROM user_blocks b WHERE (b.blocker_id=$1 AND b.blocked_id=$2) OR (b.blocker_id=$2 AND b.blocked_id=$1)),"
        "EXISTS(SELECT 1 FROM friend_requests r WHERE r.state='pending' AND ((r.sender_id=$1 AND r.receiver_id=$2) OR (r.sender_id=$2 AND r.receiver_id=$1))) FROM users u LEFT JOIN profile_display_settings s ON s.user_id=u.id WHERE u.id=$1 AND NOT u.disabled",{std::to_string(user),std::to_string(viewer)});
    if(rows.empty())throw BusinessError(Failure::NotFound,"profile not found");
    const auto&r=rows.front();PublicProfile result{user,r[0],r[1],r[2],r[3].empty()?"":"/api/files/"+r[3],viewer==user,r[4]=="t",r[5]=="t",r[6]=="t",r[7]=="t",r[8]=="t",r[9]=="t",r[10]=="t"};
    if(!result.self){if(!result.showBiography)result.biography.clear();if(!result.showAvatar)result.avatarUrl.clear();}return result;
}
void ActivityPostgresViews::configure(Id user,bool biography,bool avatar,std::optional<bool> treeRequests) {
    sql_.requireTransaction();sql_.query("INSERT INTO profile_display_settings(user_id,show_biography,show_avatar,allow_tree_friend_requests) VALUES($1,$2,$3,COALESCE(NULLIF($4,'')::boolean,false)) ON CONFLICT(user_id) DO UPDATE SET show_biography=EXCLUDED.show_biography,show_avatar=EXCLUDED.show_avatar,allow_tree_friend_requests=COALESCE(NULLIF($4,'')::boolean,profile_display_settings.allow_tree_friend_requests)",{std::to_string(user),biography?"true":"false",avatar?"true":"false",treeRequests?(*treeRequests?"true":"false"):""});
}
}
