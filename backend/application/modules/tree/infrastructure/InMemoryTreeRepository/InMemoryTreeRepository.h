#pragma once
#include "backend/application/modules/tree/ports/ITreeRepository/ITreeRepository.h"
#include "backend/application/modules/activities/ports/IContentAudience/IContentAudience.h"
#include <mutex>
#include <set>

namespace webserver::function3 {
/** 契约测试与离线开发适配器；snapshot 回滚证明业务数据和事件的事务边界。 */
class InMemoryTreeRepository final : public ITreeRepository, public IUnitOfWork,
    public IActorDirectory, public ITreeMediaCatalog, public IEventWriter, public IContentAudience {
public:
    void transact(const std::function<void()> &operation) override;
    Actor requireActor(Id user) const override;
    TreeMedia requireOwnedReadyMedia(Id owner, Id file) const override;
    void appendEvent(DomainEvent event) override;
    bool categoryActive(Id category) const override;
    std::vector<TreeCategory> categories() const override;
    std::vector<TreeCategory>allCategories()const override;
    TreeCategory createCategory(std::string name)override;
    bool updateCategory(Id category,std::string name,bool active)override;
    std::optional<TreePost> post(Id id, Id viewer) const override;
    std::optional<TreePost> postByKey(Id author, std::string_view key) const override;
    TreePost insertPost(TreePost post) override;
    void savePost(const TreePost &post) override;
    std::vector<TreePost> posts(const PostQuery &query, Id viewer) const override;
    std::uint64_t publishedCount() const override;
    bool setLike(Id post, Id user, bool liked) override;
    std::optional<TreeReply> replyByKey(Id author, std::string_view key) const override;
    TreeReply insertReply(TreeReply reply) override;
    std::optional<TreeReply> reply(Id id) const override;
    void deleteReply(Id id) override;
    std::vector<TreeReply> replies(Id post, Id before, std::size_t limit) const override;

    void seedActor(Actor actor);
    bool friendsCanView(Id author,Id viewer)const override;
    void seedFriendship(Id first,Id second);
    void seedCategory(TreeCategory category);
    void seedMedia(Id owner, TreeMedia media);
    std::vector<DomainEvent> events() const;
    void failNextEvent() { std::lock_guard lock(mutex_); failEvent_ = true; }
private:
    void requireTransaction() const;
    struct State {
        Id nextPost{1}, nextReply{1}, nextEvent{1};
        std::map<Id, Actor> actors;
        std::map<Id, TreeCategory> categories;
        std::map<Id, std::pair<Id, TreeMedia>> media;
        std::map<Id, TreePost> posts;
        std::map<Id, TreeReply> replies;
        std::set<std::pair<Id, Id>> likes;
        std::set<std::pair<Id, Id>> friendships;
        std::vector<DomainEvent> events;
    } state_;
    mutable std::recursive_mutex mutex_;
    bool transaction_{};
    bool failEvent_{};
};
}
