#pragma once
#include "backend/application/modules/activities/ports/IActivityBookmarks/IActivityBookmarks.h"
#include "backend/application/modules/activities/ports/IProfileDirectory/IProfileDirectory.h"
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
namespace webserver::function3 {
/** 只存收藏和公开展示设置；原用户/正文 ID 不变。查询投影不返回凭据字段。 */
class ActivityPostgresViews final : public IActivityBookmarks,public IProfileDirectory {
public:
    explicit ActivityPostgresViews(SqlContext&sql):sql_(sql){}
    void set(Id user,Id post,bool enabled,Milliseconds now)override;
    std::set<Id> contained(Id user,const std::vector<Id>&posts)const override;
    std::vector<Id> page(Id user,Id before,std::size_t limit)const override;
    PublicProfile profile(Id viewer,Id user)const override;
    void configure(Id user,bool biography,bool avatar,std::optional<bool> treeRequests = {})override;
private:SqlContext&sql_;
};
}
