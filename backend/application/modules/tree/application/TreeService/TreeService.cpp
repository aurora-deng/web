#include "backend/application/modules/tree/domain/TreeContentPolicy/TreeContentPolicy.h"
#include "TreeService.h"
#include "backend/application/modules/shared/TextRules/TextRules.h"
#include <algorithm>
#include <set>

namespace webserver::function3 {
namespace {
void pageSize(std::size_t limit) {
    if (!limit || limit > 100) throw BusinessError(Failure::Invalid, "page limit must be 1..100");
}
std::string canonical(const PublishPost &request) {
    // 长度前缀编码不依赖散列碰撞：同一个键只能重试完全相同的规范化载荷。
    std::string result;
    auto append = [&](std::string_view value) { result += std::to_string(value.size()) + ":" + std::string(value); };
    append(request.body); append(std::to_string(request.category)); append(request.mood);
    append(std::to_string(request.tags.size()));
    for (const auto &tag : request.tags) append(tag);
    append(std::to_string(request.files.size()));
    for (Id file : request.files) append(std::to_string(file));
    // 兼容旧果实的幂等签名；新频道组合必须参与签名，不能借重试扩大可见范围。
    if (!request.treeChannel || !request.profileChannel || request.visibility != "members") {
        append(request.treeChannel ? "tree" : "no-tree");
        append(request.profileChannel ? "profile" : "no-profile");append(request.visibility);
    }
    return result;
}
}
TreePost TreeService::requirePublished(Id id, Id viewer) const {
    auto post = repository_.post(id, viewer);
    if (!post || post->state != PostState::Published) throw BusinessError(Failure::NotFound, "fruit not found");
    if (post->author != viewer && !(post->content.treeChannel && post->content.visibility == "members") &&
        !(post->content.visibility != "private" && audience_ && audience_->friendsCanView(post->author,viewer)))
        throw BusinessError(Failure::NotFound,"activity not found");
    return *post;
}
void TreeService::changed(const TreePost &post, std::string type, Milliseconds now) {
    // 非树频道不广播给全站；正文也不进入公共事件载荷。
    if (!post.content.treeChannel) type = "activity.private.changed";
    events_.appendEvent({0, std::move(type), "tree.post", post.id, post.version,
        {{"postId", std::to_string(post.id)}, {"authorId", std::to_string(post.author)}}, now});
}
TreePost TreeService::publish(Id user, PublishPost request, Milliseconds now) {
    requestKey(request.requestId);
    request.body = trimText(std::move(request.body));
    request.mood = trimText(std::move(request.mood));
    textLength(request.body, 2000); textLength(request.mood, 16);
    if ((!request.treeChannel && !request.profileChannel) ||
        (request.visibility != "members" && request.visibility != "friends" && request.visibility != "private") ||
        (request.treeChannel && request.visibility != "members"))
        throw BusinessError(Failure::Invalid,"invalid channels or audience");
    if (request.tags.size() > 5 || request.files.size() > 7)
        throw BusinessError(Failure::Invalid, "too many tags or files");
    for (auto &tag : request.tags) {
        tag = trimText(std::move(tag));
        if (!textLength(tag, 16)) throw BusinessError(Failure::Invalid, "empty tag");
    }
    std::sort(request.tags.begin(), request.tags.end());
    request.tags.erase(std::unique(request.tags.begin(), request.tags.end()), request.tags.end());
    const auto signature = canonical(request);
    TreePost result;
    work_.transact([&] {
        const auto actor = activeActor(actors_, user);
        if (const auto previous = repository_.postByKey(user, request.requestId)) {
            if (previous->canonicalRequest != signature) throw BusinessError(Failure::Conflict, "idempotency key payload differs");
            result = *previous; return;
        }
        if ((request.treeChannel || request.category) && !repository_.categoryActive(request.category)) throw BusinessError(Failure::Invalid, "category unavailable");
        std::vector<TreeMedia> attachments;
        std::set<Id> uniqueFiles;
        std::size_t images = 0, audio = 0;
        for (Id file : request.files) {
            if (!uniqueFiles.insert(file).second) throw BusinessError(Failure::Invalid, "duplicate attachment");
            auto value = media_.requireOwnedReadyMedia(user, file);
            if (!value.bytes) throw BusinessError(Failure::Invalid, "empty attachment");
            if (value.kind == MediaKind::Image) {
                if (++images > TreeContentPolicy::images || value.bytes > TreeContentPolicy::imageBytes) throw BusinessError(Failure::Invalid, "image limit exceeded");
            } else if (++audio > TreeContentPolicy::voices || value.bytes > TreeContentPolicy::voiceBytes || value.duration <= 0 || value.duration > TreeContentPolicy::voiceMilliseconds)
                throw BusinessError(Failure::Invalid, "audio limit exceeded");
            attachments.push_back(value);
        }
        if (request.body.empty() && attachments.empty()) throw BusinessError(Failure::Invalid, "fruit content required");
        TreePost post;
        post.author = user; post.authorName = actor.name; post.content = request;
        post.media = std::move(attachments); post.canonicalRequest = signature; post.createdAt = now;
        result = repository_.insertPost(std::move(post));
        changed(result, "tree.post.created", now);
    });
    return capabilities(std::move(result),activeActor(actors_,user));
}
TreePost TreeService::get(Id actor, Id post) const {
    const auto viewer=activeActor(actors_,actor);
    return capabilities(requirePublished(post, actor),viewer);
}
TreePost TreeService::capabilities(TreePost post, const Actor &viewer) const {
    post.canDelete = post.author == viewer.id || viewer.role == Role::Administrator;
    post.deleteReasonRequired = post.author != viewer.id && viewer.role == Role::Administrator;
    post.canHide = viewer.role != Role::User;
    return post;
}
std::vector<TreePost> TreeService::list(Id actor, PostQuery query) const {
    const auto viewer = activeActor(actors_, actor); pageSize(query.limit);textLength(query.tag,16);textLength(query.mood,16);
    if(query.ids.size()>100||std::find(query.ids.begin(),query.ids.end(),Id{0})!=query.ids.end())
        throw BusinessError(Failure::Invalid,"invalid post batch");
    if ((query.profileOnly || query.allChannels) && !query.author) throw BusinessError(Failure::Invalid,"profile author required");
    if((query.since&&*query.since<0)||(query.until&&*query.until<=0)||(query.since&&query.until&&*query.since>=*query.until))throw BusinessError(Failure::Invalid,"invalid time range");
    auto posts = repository_.posts(query, actor);
    std::erase_if(posts,[&](const auto &p){return p.author!=actor && !(p.content.treeChannel && p.content.visibility=="members") &&
        !(p.content.visibility!="private" && audience_ && audience_->friendsCanView(p.author,actor));});
    for (auto &post : posts) post = capabilities(std::move(post), viewer);
    return posts;
}
std::vector<TreeCategory> TreeService::categories(Id actor) const {
    activeActor(actors_, actor); return repository_.categories();
}
std::uint64_t TreeService::count(Id actor) const {
    activeActor(actors_, actor); return repository_.publishedCount();
}
std::vector<TreeCategory>TreeService::adminCategories(Id actor)const{moderator(activeActor(actors_,actor));return repository_.allCategories();}
TreeCategory TreeService::category(Id actor,Id id,std::string name,bool active,Milliseconds now){
    name=trimText(std::move(name));if(!textLength(name,20))throw BusinessError(Failure::Invalid,"category name required");TreeCategory result;
    work_.transact([&]{moderator(activeActor(actors_,actor));if(id){if(!repository_.updateCategory(id,name,active))throw BusinessError(Failure::NotFound,"category not found");result={id,name,active};}else{result=repository_.createCategory(name);if(!active){repository_.updateCategory(result.id,name,false);result.active=false;}}
        events_.appendEvent({0,"admin.audit","tree.category",result.id,static_cast<std::uint64_t>(now),{{"actorId",std::to_string(actor)},{"action","category"},{"reason",name+(active?" enabled":" disabled")}},now});});return result;
}
TreePost TreeService::like(Id user, Id id, bool liked, Milliseconds now) {
    TreePost result;
    work_.transact([&] {
        activeActor(actors_, user); result = requirePublished(id, user);
        if (repository_.setLike(id, user, liked)) {
            result = requirePublished(id, user); ++result.version; repository_.savePost(result);
            changed(result, "tree.post.updated", now);
        }
    });
    return result;
}
TreeReply TreeService::reply(Id user, Id id, std::string key, std::string body, Milliseconds now, Id parent) {
    requestKey(key); body = trimText(std::move(body));
    if (!textLength(body, 1000)) throw BusinessError(Failure::Invalid, "reply required");
    TreeReply result;
    work_.transact([&] {
        const auto actor = activeActor(actors_, user); auto post = requirePublished(id, user);
        if (auto old = repository_.replyByKey(user, key)) {
            if (old->post != id || old->body != body || old->parent != parent) throw BusinessError(Failure::Conflict, "reply idempotency key differs");
            result = *old; return;
        }
        std::string targetName;
        if(parent){auto target=repository_.reply(parent);if(!target||target->deleted||target->post!=id)throw BusinessError(Failure::Invalid,"reply target must belong to this post");targetName=target->authorName;}
        result = repository_.insertReply({0, id, user, actor.name, key, body, now, false, parent, targetName});
        post = requirePublished(id, user); ++post.version; repository_.savePost(post);
        changed(post, "tree.post.updated", now);
    });
    return result;
}
std::vector<TreeReply> TreeService::replies(Id user, Id id, Id before, std::size_t limit) const {
    activeActor(actors_, user); requirePublished(id, user); pageSize(limit);
    return repository_.replies(id, before, limit);
}
void TreeService::removeReply(Id user, Id id, Milliseconds now) {
    work_.transact([&] {
        const auto actor = activeActor(actors_, user); auto reply = repository_.reply(id);
        if (!reply) throw BusinessError(Failure::NotFound, "reply not found");
        if (reply->author != user && actor.role == Role::User) throw BusinessError(Failure::Forbidden, "cannot delete another reply");
        if (reply->deleted) return;
        auto post = requirePublished(reply->post, user); repository_.deleteReply(id);
        post = requirePublished(reply->post, user); ++post.version; repository_.savePost(post);
        changed(post, "tree.post.updated", now);
    });
}
void TreeService::remove(Id user, Id id, Milliseconds now, std::string reason) {
    reason = trimText(std::move(reason));
    work_.transact([&] {
        const auto actor = activeActor(actors_, user); const auto value = repository_.post(id, user);
        if (!value) throw BusinessError(Failure::NotFound, "fruit not found");
        const bool administration = value->author != user;
        if (administration && actor.role != Role::Administrator)
            throw BusinessError(Failure::Forbidden, "author or site administrator required");
        if (administration && !textLength(reason, 200))
            throw BusinessError(Failure::Invalid, "administrator deletion reason required");
        if (value->state == PostState::Deleted) return;
        auto post = *value; post.state = PostState::Deleted; ++post.version;
        repository_.savePost(post); changed(post, "tree.post.removed", now);
        if (administration) events_.appendEvent({0,"admin.audit","tree.post",id,post.version,
            {{"actorId",std::to_string(user)},{"reason",reason},{"action","delete"}},now});
    });
}
void TreeService::hide(Id user, Id id, std::string reason, Milliseconds now) {
    reason = trimText(std::move(reason));
    if (!textLength(reason, 200)) throw BusinessError(Failure::Invalid, "moderation reason required");
    work_.transact([&] {
        moderator(activeActor(actors_, user)); auto post = requirePublished(id, user);
        post.state = PostState::Hidden; ++post.version; repository_.savePost(post);
        changed(post, "tree.post.removed", now);
        events_.appendEvent({0, "admin.audit", "tree.post", id, post.version,
            {{"actorId", std::to_string(user)}, {"reason", reason}, {"action", "hide"}}, now});
    });
}
}
