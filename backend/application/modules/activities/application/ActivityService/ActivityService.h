#pragma once
#include "backend/application/modules/activities/ports/IActivityContent/IActivityContent.h"
#include "backend/application/modules/activities/ports/IActivityBookmarks/IActivityBookmarks.h"
#include "backend/application/modules/activities/ports/IProfileDirectory/IProfileDirectory.h"
namespace webserver::function3 {
struct FavoritePage {std::vector<TreePost> items;Id nextCursor{};};
/** 频道/主页/收藏编排；正文与互动仍走同一个内容 Port，避免两份动态分叉。 */
class ActivityService final {
public:
    ActivityService(IUnitOfWork&work,IActorDirectory&actors,IActivityContent&content,
        IActivityBookmarks&bookmarks,IProfileDirectory&profiles)
        :work_(work),actors_(actors),content_(content),bookmarks_(bookmarks),profiles_(profiles){}
    TreePost publish(Id actor,PublishPost content,bool publicConfirmed,Milliseconds now);
    TreePost get(Id actor,Id post)const;
    std::vector<TreePost> profileActivities(Id actor,Id owner,Id before,bool treeOnly,std::size_t limit)const;
    TreePost favorite(Id actor,Id post,bool enabled,Milliseconds now);
    FavoritePage favorites(Id actor,Id before,std::size_t limit)const;
    std::string share(Id actor,Id post)const;
    PublicProfile profile(Id actor,Id user)const;
    PublicProfile treeAuthor(Id actor,Id post)const;
    Id treeFriendTarget(Id actor,Id post)const;
    void configureProfile(Id actor,bool biography,bool avatar,std::optional<bool> treeRequests = {});
private:
    void decorate(Id actor,std::vector<TreePost>&posts)const;
    IUnitOfWork&work_;IActorDirectory&actors_;IActivityContent&content_;
    IActivityBookmarks&bookmarks_;IProfileDirectory&profiles_;
};
}
