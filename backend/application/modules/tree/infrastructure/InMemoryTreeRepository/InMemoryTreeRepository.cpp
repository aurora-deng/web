#include "InMemoryTreeRepository.h"
#include <algorithm>

namespace webserver::function3 {
bool InMemoryTreeRepository::friendsCanView(Id author,Id viewer)const {
    std::lock_guard lock(mutex_);return state_.friendships.contains({std::min(author,viewer),std::max(author,viewer)});
}
void InMemoryTreeRepository::seedFriendship(Id first,Id second) {
    std::lock_guard lock(mutex_);state_.friendships.emplace(std::min(first,second),std::max(first,second));
}
void InMemoryTreeRepository::requireTransaction() const {
    if (!transaction_) throw std::logic_error("write requires transaction");
}
void InMemoryTreeRepository::transact(const std::function<void()> &operation) {
    std::lock_guard lock(mutex_);
    if (transaction_) throw std::logic_error("nested transaction is unsupported");
    auto snapshot = state_; transaction_ = true;
    try { operation(); transaction_ = false; }
    catch (...) { state_ = std::move(snapshot); transaction_ = false; throw; }
}
Actor InMemoryTreeRepository::requireActor(Id user) const {
    std::lock_guard lock(mutex_);
    const auto found = state_.actors.find(user);
    if (found == state_.actors.end()) throw BusinessError(Failure::Unauthenticated, "user not found");
    return found->second;
}
TreeMedia InMemoryTreeRepository::requireOwnedReadyMedia(Id user, Id file) const {
    std::lock_guard lock(mutex_); const auto found = state_.media.find(file);
    if (found == state_.media.end() || found->second.first != user)
        throw BusinessError(Failure::Forbidden, "media ownership or readiness required");
    return found->second.second;
}
void InMemoryTreeRepository::appendEvent(DomainEvent event) {
    std::lock_guard lock(mutex_); requireTransaction();
    if (failEvent_) { failEvent_ = false; throw BusinessError(Failure::Busy, "injected event failure"); }
    event.id = state_.nextEvent++; state_.events.push_back(std::move(event));
}
bool InMemoryTreeRepository::categoryActive(Id category) const {
    std::lock_guard lock(mutex_); auto it = state_.categories.find(category);
    return it != state_.categories.end() && it->second.active;
}
std::vector<TreeCategory>InMemoryTreeRepository::allCategories()const{std::lock_guard lock(mutex_);std::vector<TreeCategory>result;for(auto &[_,value]:state_.categories)result.push_back(value);return result;}
TreeCategory InMemoryTreeRepository::createCategory(std::string name){std::lock_guard lock(mutex_);requireTransaction();for(auto &[_,c]:state_.categories)if(c.name==name)throw BusinessError(Failure::Conflict,"category exists");Id id=state_.categories.empty()?1:state_.categories.rbegin()->first+1;TreeCategory value{id,std::move(name),true};state_.categories[id]=value;return value;}
bool InMemoryTreeRepository::updateCategory(Id id,std::string name,bool active){std::lock_guard lock(mutex_);requireTransaction();auto i=state_.categories.find(id);if(i==state_.categories.end())return false;for(auto &[other,c]:state_.categories)if(other!=id&&c.name==name)throw BusinessError(Failure::Conflict,"category exists");i->second={id,std::move(name),active};return true;}
std::vector<TreeCategory> InMemoryTreeRepository::categories() const {
    std::lock_guard lock(mutex_); std::vector<TreeCategory> result;
    for (const auto &[_, value] : state_.categories) if (value.active) result.push_back(value);
    return result;
}
std::optional<TreePost> InMemoryTreeRepository::post(Id id, Id viewer) const {
    std::lock_guard lock(mutex_); const auto found = state_.posts.find(id);
    if (found == state_.posts.end()) return {};
    auto result = found->second; result.likes = 0; result.repliers = 0;
    result.likedByViewer = state_.likes.contains({id, viewer});
    for (auto [post, user] : state_.likes) if (post == id && user != result.author) ++result.likes;
    std::set<Id> authors;
    for (const auto &[_, reply] : state_.replies)
        if (reply.post == id && !reply.deleted && reply.author != result.author) authors.insert(reply.author);
    result.repliers = authors.size(); return result;
}
std::optional<TreePost> InMemoryTreeRepository::postByKey(Id user, std::string_view key) const {
    std::lock_guard lock(mutex_);
    for (const auto &[_, value] : state_.posts)
        if (value.author == user && value.content.requestId == key) return post(value.id, user);
    return {};
}
TreePost InMemoryTreeRepository::insertPost(TreePost value) {
    std::lock_guard lock(mutex_); requireTransaction();
    if (postByKey(value.author, value.content.requestId)) throw BusinessError(Failure::Conflict, "duplicate key");
    value.id = state_.nextPost++; state_.posts[value.id] = value; return value;
}
void InMemoryTreeRepository::savePost(const TreePost &value) {
    std::lock_guard lock(mutex_); requireTransaction(); state_.posts.at(value.id) = value;
}
std::vector<TreePost> InMemoryTreeRepository::posts(const PostQuery &q, Id viewer) const {
    std::lock_guard lock(mutex_); std::vector<TreePost> result;
    for (auto it = state_.posts.rbegin(); it != state_.posts.rend() && result.size() < q.limit; ++it) {
        const auto &p = it->second;
        if(!q.ids.empty()&&std::find(q.ids.begin(),q.ids.end(),p.id)==q.ids.end())continue;
        if (!q.allChannels && (q.profileOnly ? !p.content.profileChannel : !p.content.treeChannel)) continue;
        if (p.author!=viewer && !(p.content.treeChannel && p.content.visibility=="members") &&
            !(p.content.visibility!="private" && friendsCanView(p.author,viewer))) continue;
        if (p.state != PostState::Published || (q.before && p.id >= q.before) ||
            (q.since && p.createdAt < *q.since) || (q.until && p.createdAt >= *q.until) ||
            (q.author && p.author != *q.author) || (q.category && p.content.category != *q.category) ||
            (!q.mood.empty() && p.content.mood != q.mood) || (!q.tag.empty() &&
             std::find(p.content.tags.begin(), p.content.tags.end(), q.tag) == p.content.tags.end())) continue;
        if (q.mediaKind && std::none_of(p.media.begin(), p.media.end(), [&](const auto &m) { return m.kind == *q.mediaKind; })) continue;
        result.push_back(*post(p.id, viewer));
    }
    return result;
}
std::uint64_t InMemoryTreeRepository::publishedCount() const {
    std::lock_guard lock(mutex_);
    return std::count_if(state_.posts.begin(), state_.posts.end(), [](const auto &pair) { return pair.second.state == PostState::Published && pair.second.content.treeChannel; });
}
bool InMemoryTreeRepository::setLike(Id post, Id user, bool liked) {
    std::lock_guard lock(mutex_); requireTransaction();
    return liked ? state_.likes.insert({post, user}).second : state_.likes.erase({post, user}) != 0;
}
std::optional<TreeReply> InMemoryTreeRepository::replyByKey(Id user, std::string_view key) const {
    std::lock_guard lock(mutex_);
    for (const auto &[_, value] : state_.replies) if (value.author == user && value.requestId == key) return value;
    return {};
}
TreeReply InMemoryTreeRepository::insertReply(TreeReply value) {
    std::lock_guard lock(mutex_); requireTransaction();
    value.id = state_.nextReply++; state_.replies[value.id] = value; return value;
}
std::optional<TreeReply> InMemoryTreeRepository::reply(Id id) const {
    std::lock_guard lock(mutex_); const auto found = state_.replies.find(id);
    return found == state_.replies.end() ? std::nullopt : std::optional<TreeReply>{found->second};
}
void InMemoryTreeRepository::deleteReply(Id id) {
    std::lock_guard lock(mutex_); requireTransaction(); state_.replies.at(id).deleted = true;
}
std::vector<TreeReply> InMemoryTreeRepository::replies(Id post, Id before, std::size_t limit) const {
    std::lock_guard lock(mutex_); std::vector<TreeReply> result;
    for (auto it = state_.replies.rbegin(); it != state_.replies.rend() && result.size() < limit; ++it)
        if (it->second.post == post && !it->second.deleted && (!before || it->first < before)) result.push_back(it->second);
    return result;
}
void InMemoryTreeRepository::seedActor(Actor actor) { std::lock_guard lock(mutex_); state_.actors[actor.id] = std::move(actor); }
void InMemoryTreeRepository::seedCategory(TreeCategory value) { std::lock_guard lock(mutex_); state_.categories[value.id] = std::move(value); }
void InMemoryTreeRepository::seedMedia(Id owner, TreeMedia value) { std::lock_guard lock(mutex_); state_.media[value.file] = {owner, value}; }
std::vector<DomainEvent> InMemoryTreeRepository::events() const { std::lock_guard lock(mutex_); return state_.events; }
}
