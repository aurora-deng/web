#pragma once
#include "backend/application/modules/tree/ports/ITreeRepository/ITreeRepository.h"
#include "backend/application/modules/activities/ports/IActivityContent/IActivityContent.h"
#include "backend/application/modules/activities/ports/IContentAudience/IContentAudience.h"

namespace webserver::function3 {
/** 发布、互动和权限只在这里判断；协议和数据库适配器不复制业务规则。 */
class TreeService final : public IActivityContent {
public:
    TreeService(IUnitOfWork &work, IActorDirectory &actors, ITreeRepository &repository,
                ITreeMediaCatalog &media, IEventWriter &events, IContentAudience *audience = nullptr)
        : work_(work), actors_(actors), repository_(repository), media_(media), events_(events), audience_(audience) {}
    TreePost publish(Id actor, PublishPost request, Milliseconds now);
    TreePost get(Id actor, Id post) const;
    std::vector<TreePost> list(Id actor, PostQuery query) const;
    std::vector<TreeCategory> categories(Id actor) const;
    std::uint64_t count(Id actor) const;
    std::vector<TreeCategory>adminCategories(Id actor)const;
    TreeCategory category(Id actor,Id category,std::string name,bool active,Milliseconds now);
    TreePost like(Id actor, Id post, bool liked, Milliseconds now);
    TreeReply reply(Id actor, Id post, std::string key, std::string body, Milliseconds now, Id parent=0);
    std::vector<TreeReply> replies(Id actor, Id post, Id before, std::size_t limit) const;
    void removeReply(Id actor, Id reply, Milliseconds now);
    void remove(Id actor, Id post, Milliseconds now, std::string reason = {});
    void hide(Id actor, Id post, std::string reason, Milliseconds now);
private:
    TreePost requirePublished(Id post, Id viewer) const;
    TreePost capabilities(TreePost post, const Actor &viewer) const;
    void changed(const TreePost &post, std::string type, Milliseconds now);
    IUnitOfWork &work_;
    IActorDirectory &actors_;
    ITreeRepository &repository_;
    ITreeMediaCatalog &media_;
    IEventWriter &events_;
    IContentAudience *audience_{};
};
}
