#include "ActivityService.h"
#include "backend/application/modules/shared/TextRules/TextRules.h"
namespace webserver::function3 {
namespace {void pageSize(std::size_t value){if(value==0||value>100)throw BusinessError(Failure::Invalid,"page size must be 1..100");}}
TreePost ActivityService::publish(Id actor,PublishPost request,bool publicConfirmed,Milliseconds now) {
    activeActor(actors_,actor);
    if(request.treeChannel && !publicConfirmed)
        throw BusinessError(Failure::Invalid,"确认苦情树内容对所有登录用户可见后再发布");
    return content_.publish(actor,std::move(request),now);
}
void ActivityService::decorate(Id actor,std::vector<TreePost>&posts)const {
    std::vector<Id> ids;for(const auto&p:posts)ids.push_back(p.id);
    const auto contained=bookmarks_.contained(actor,ids);
    for(auto&p:posts)p.favoritedByViewer=contained.contains(p.id);
}
TreePost ActivityService::get(Id actor,Id id)const {
    auto post=content_.get(actor,id);std::vector<TreePost> posts{post};decorate(actor,posts);return posts.front();
}
std::vector<TreePost> ActivityService::profileActivities(Id actor,Id owner,Id before,bool treeOnly,std::size_t limit)const {
    activeActor(actors_,actor);const auto ownerProfile=profiles_.profile(actor,owner);pageSize(limit);
    PostQuery q;q.author=owner;q.before=before;q.limit=limit;q.allChannels=!treeOnly && (ownerProfile.self || (ownerProfile.friendship && !ownerProfile.blocked));
    auto result=content_.list(actor,q);decorate(actor,result);return result;
}
TreePost ActivityService::favorite(Id actor,Id post,bool enabled,Milliseconds now) {
    work_.transact([&]{activeActor(actors_,actor);content_.get(actor,post);bookmarks_.set(actor,post,enabled,now);});
    return get(actor,post);
}
FavoritePage ActivityService::favorites(Id actor,Id before,std::size_t limit)const {
    activeActor(actors_,actor);pageSize(limit);FavoritePage result;
    const auto ids=bookmarks_.page(actor,before,limit);
    for(Id id:ids) {
        result.nextCursor=id;
        try {result.items.push_back(content_.get(actor,id));}
        catch(const BusinessError&e){if(e.code!=Failure::NotFound)throw;}
    }
    if(ids.size()<limit)result.nextCursor=0;
    decorate(actor,result.items);return result;
}
std::string ActivityService::share(Id actor,Id id)const {
    const auto p=content_.get(actor,id);
    return p.content.treeChannel?"/phase11/tree/?post="+std::to_string(id):
        "/phase11/profile/?user="+std::to_string(p.author)+"&post="+std::to_string(id);
}
PublicProfile ActivityService::profile(Id actor,Id user)const {
    activeActor(actors_,actor);return profiles_.profile(actor,user);
}
PublicProfile ActivityService::treeAuthor(Id actor,Id post)const {
    const auto content=get(actor,post);
    if(!content.content.treeChannel)throw BusinessError(Failure::NotFound,"tree fruit not found");
    return profile(actor,content.author);
}
Id ActivityService::treeFriendTarget(Id actor,Id post)const {
    const auto author=treeAuthor(actor,post);
    if(author.self)throw BusinessError(Failure::Invalid,"cannot request yourself");
    if(!author.allowTreeFriendRequests || author.blocked)throw BusinessError(Failure::Forbidden,"作者尚未开启通过果实添加好友");
    if(author.friendship || author.pendingRequest)throw BusinessError(Failure::Conflict,"已经是好友或已有待处理申请");
    return author.id;
}
void ActivityService::configureProfile(Id actor,bool biography,bool avatar,std::optional<bool> treeRequests) {
    work_.transact([&]{activeActor(actors_,actor);profiles_.configure(actor,biography,avatar,treeRequests);});
}
}
