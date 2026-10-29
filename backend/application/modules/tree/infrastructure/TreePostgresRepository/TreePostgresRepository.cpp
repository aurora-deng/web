#include "TreePostgresRepository.h"
#include <unordered_map>

namespace webserver::function3 {
namespace {
std::string str(Id value) { return std::to_string(value); }
TreeReply parseReply(const std::vector<std::string> &r) {
    return {std::stoull(r[0]),std::stoull(r[1]),std::stoull(r[2]),r[3],r[4],r[5],std::stoll(r[6]),r[7]=="t",std::stoull(r[8]),r[9]};
}
const std::string replySelect="SELECT r.id,r.post_id,r.author_id,u.display_name,r.request_key,r.body,r.created_ms,r.deleted,COALESCE(l.parent_reply_id,0),COALESCE(pu.display_name,'') FROM tree_replies r JOIN users u ON u.id=r.author_id LEFT JOIN tree_reply_links l ON l.reply_id=r.id LEFT JOIN tree_replies pr ON pr.id=l.parent_reply_id LEFT JOIN users pu ON pu.id=pr.author_id ";
// 列表和详情共享同一投影；查看者参数编号由调用者给出，值始终参数化。
std::string postSelect(const std::string& viewer) {
    return "SELECT p.id,p.author_id,u.display_name,p.request_key,p.canonical_request,p.body,COALESCE(p.category_id,0),p.mood,p.state,p.created_ms,p.version,"
        "(SELECT count(*) FROM tree_likes l WHERE l.post_id=p.id AND l.user_id<>p.author_id),"
        "(SELECT count(DISTINCT r.author_id) FROM tree_replies r WHERE r.post_id=p.id AND NOT r.deleted AND r.author_id<>p.author_id),"
        "EXISTS(SELECT 1 FROM tree_likes l WHERE l.post_id=p.id AND l.user_id="+viewer+"),p.tree_channel,p.profile_channel,p.visibility FROM tree_posts p JOIN users u ON u.id=p.author_id ";
}
std::string idArray(const std::vector<TreePost>& posts) {
    std::string result="{";for(const auto& p:posts){if(result.size()>1)result+=',';result+=str(p.id);}return result+"}";
}
}
bool TreePostgresRepository::categoryActive(Id id) const {
    return !sql_.query("SELECT 1 FROM tree_categories WHERE id=$1 AND active",{str(id)}).empty();
}
bool TreePostgresRepository::friendsCanView(Id author,Id viewer)const {
    return !sql_.query("SELECT 1 FROM friendships WHERE user_low_id=LEAST($1::bigint,$2::bigint) AND user_high_id=GREATEST($1::bigint,$2::bigint) "
        "AND NOT EXISTS(SELECT 1 FROM user_blocks WHERE (blocker_id=$1 AND blocked_id=$2) OR (blocker_id=$2 AND blocked_id=$1))",
        {str(author),str(viewer)}).empty();
}
std::vector<TreeCategory>TreePostgresRepository::allCategories()const{std::vector<TreeCategory>result;for(auto&r:sql_.query("SELECT id,name,active FROM tree_categories ORDER BY id"))result.push_back({std::stoull(r[0]),r[1],r[2]=="t"});return result;}
TreeCategory TreePostgresRepository::createCategory(std::string name){sql_.requireTransaction();auto r=sql_.query("INSERT INTO tree_categories(name) VALUES($1) RETURNING id",{name});return{std::stoull(r[0][0]),std::move(name),true};}
bool TreePostgresRepository::updateCategory(Id id,std::string name,bool active){sql_.requireTransaction();return !sql_.query("UPDATE tree_categories SET name=$2,active=$3 WHERE id=$1 RETURNING id",{std::to_string(id),std::move(name),active?"true":"false"}).empty();}
std::vector<TreeCategory> TreePostgresRepository::categories() const {
    std::vector<TreeCategory> result;
    for (auto &r:sql_.query("SELECT id,name,active FROM tree_categories WHERE active ORDER BY id")) result.push_back({std::stoull(r[0]),r[1],true});
    return result;
}
std::optional<TreePost> TreePostgresRepository::post(Id id,Id viewer) const {
    // 写事务锁住单个果实：点赞/删除/回复不能用旧版本覆盖对方状态。
    if (sql_.inTransaction()) sql_.query("SELECT id FROM tree_posts WHERE id=$1 FOR UPDATE",{str(id)});
    auto posts=hydrate(sql_.query(postSelect("$2")+"WHERE p.id=$1",{str(id),str(viewer)}));
    return posts.empty()?std::optional<TreePost>{}:std::move(posts.front());
}
std::vector<TreePost> TreePostgresRepository::hydrate(const SqlRows& rows) const {
    std::vector<TreePost> result;result.reserve(rows.size());
    std::unordered_map<Id,std::size_t> positions;
    for(const auto& r:rows){TreePost p;
    p.id=std::stoull(r[0]); p.author=std::stoull(r[1]); p.authorName=r[2]; p.content.requestId=r[3]; p.canonicalRequest=r[4];
    p.content.body=r[5]; p.content.category=std::stoull(r[6]); p.content.mood=r[7];
    p.state=r[8]=="published"?PostState::Published:r[8]=="hidden"?PostState::Hidden:PostState::Deleted;
    p.createdAt=std::stoll(r[9]); p.version=std::stoull(r[10]); p.likes=std::stoull(r[11]); p.repliers=std::stoull(r[12]); p.likedByViewer=r[13]=="t";
    p.content.treeChannel=r[14]=="t";p.content.profileChannel=r[15]=="t";p.content.visibility=r[16];
    positions.emplace(p.id,result.size());result.push_back(std::move(p));}
    if(result.empty())return result;
    // 一页最多100条：正文、标签、媒体共3次SQL，代替原来的1+3N次往返。
    // 不通过字符串分隔符打包用户标签，保留中文、逗号和引号的原始语义。
    const std::vector<std::string> ids{idArray(result)};
    for(const auto& t:sql_.query("SELECT post_id,tag FROM tree_tags WHERE post_id=ANY($1::bigint[]) ORDER BY post_id,tag",ids))
        result.at(positions.at(std::stoull(t[0]))).content.tags.push_back(t[1]);
    for(const auto& m:sql_.query("SELECT m.post_id,v.file_id,v.kind,v.bytes,v.duration_ms FROM tree_media m JOIN tree_media_validation v ON v.file_id=m.file_id WHERE m.post_id=ANY($1::bigint[]) ORDER BY m.post_id,m.position",ids)) {
        auto& p=result.at(positions.at(std::stoull(m[0])));
        p.media.push_back({std::stoull(m[1]),m[2]=="image"?MediaKind::Image:MediaKind::Audio,std::stoull(m[3]),std::stoll(m[4])});
        p.content.files.push_back(std::stoull(m[1]));
    }
    return result;
}
std::optional<TreePost> TreePostgresRepository::postByKey(Id author,std::string_view key) const {
    if(sql_.inTransaction()) sql_.query("SELECT pg_advisory_xact_lock(hashtextextended($1,0))",{"tree.publish:"+str(author)+":"+std::string(key)});
    auto r=sql_.query("SELECT id FROM tree_posts WHERE author_id=$1 AND request_key=$2",{str(author),std::string(key)});
    return r.empty()?std::optional<TreePost>{}:post(std::stoull(r[0][0]),author);
}
TreePost TreePostgresRepository::insertPost(TreePost p) {
    sql_.requireTransaction();
    auto r=sql_.query("INSERT INTO tree_posts(author_id,request_key,canonical_request,body,category_id,mood,created_ms,tree_channel,profile_channel,visibility) VALUES($1,$2,$3,$4,NULLIF($5::bigint,0),$6,$7,$8,$9,$10) RETURNING id",{str(p.author),p.content.requestId,p.canonicalRequest,p.content.body,str(p.content.category),p.content.mood,std::to_string(p.createdAt),p.content.treeChannel?"true":"false",p.content.profileChannel?"true":"false",p.content.visibility});
    p.id=std::stoull(r[0][0]);
    for(auto &tag:p.content.tags) sql_.query("INSERT INTO tree_tags VALUES($1,$2)",{str(p.id),tag});
    for(std::size_t i=0;i<p.media.size();++i) sql_.query("INSERT INTO tree_media VALUES($1,$2,$3)",{str(p.id),str(p.media[i].file),str(i)});
    return p;
}
void TreePostgresRepository::savePost(const TreePost &p) {
    sql_.requireTransaction();
    sql_.query("UPDATE tree_posts SET state=$2,version=$3 WHERE id=$1",{str(p.id),p.state==PostState::Published?"published":p.state==PostState::Hidden?"hidden":"deleted",str(p.version)});
}
std::vector<TreePost> TreePostgresRepository::posts(const PostQuery &q,Id viewer) const {
    std::vector<std::string> args{str(q.before),str(q.limit),str(viewer)};
    std::string sql=postSelect("$3")+"WHERE p.state='published' AND ($1::bigint=0 OR p.id<$1)";
    if(!q.allChannels)sql+=q.profileOnly?" AND p.profile_channel":" AND p.tree_channel";
    sql+=" AND (p.author_id=$3 OR (p.tree_channel AND p.visibility='members') OR (p.visibility<>'private' "
        "AND EXISTS(SELECT 1 FROM friendships f WHERE f.user_low_id=LEAST(p.author_id,$3::bigint) AND f.user_high_id=GREATEST(p.author_id,$3::bigint)) "
        "AND NOT EXISTS(SELECT 1 FROM user_blocks b WHERE (b.blocker_id=p.author_id AND b.blocked_id=$3) OR (b.blocker_id=$3 AND b.blocked_id=p.author_id))))";
    auto filter=[&](std::string clause,std::string value){args.push_back(std::move(value)); sql+=" AND "+clause+"$"+str(args.size());};
    if(q.author) filter("p.author_id=",str(*q.author));
    if(q.category) filter("p.category_id=",str(*q.category));
    if(q.since) filter("p.created_ms>=",std::to_string(*q.since));
    if(q.until) filter("p.created_ms<",std::to_string(*q.until));
    if(!q.mood.empty()) filter("p.mood=",q.mood);
    if(!q.tag.empty()) {args.push_back(q.tag);sql+=" AND EXISTS(SELECT 1 FROM tree_tags t WHERE t.post_id=p.id AND t.tag=$"+str(args.size())+")";}
    if(q.mediaKind) {args.push_back(*q.mediaKind==MediaKind::Image?"image":"audio");sql+=" AND EXISTS(SELECT 1 FROM tree_media m JOIN tree_media_validation v ON v.file_id=m.file_id WHERE m.post_id=p.id AND v.kind=$"+str(args.size())+")";}
    if(!q.ids.empty()){std::string ids="{";for(auto id:q.ids){if(ids.size()>1)ids+=',';ids+=str(id);}ids+='}';args.push_back(std::move(ids));sql+=" AND p.id=ANY($"+str(args.size())+"::bigint[])";}
    sql+=" ORDER BY p.id DESC LIMIT $2";
    if(sql_.inTransaction())sql+=" FOR UPDATE OF p";
    return hydrate(sql_.query(sql,args));
}
std::uint64_t TreePostgresRepository::publishedCount() const {return std::stoull(sql_.query("SELECT count(*) FROM tree_posts WHERE state='published' AND tree_channel")[0][0]);}
bool TreePostgresRepository::setLike(Id post,Id user,bool liked) {
    sql_.requireTransaction();
    return !sql_.query(liked?"INSERT INTO tree_likes VALUES($1,$2) ON CONFLICT DO NOTHING RETURNING post_id":"DELETE FROM tree_likes WHERE post_id=$1 AND user_id=$2 RETURNING post_id",{str(post),str(user)}).empty();
}
std::optional<TreeReply> TreePostgresRepository::replyByKey(Id user,std::string_view key) const {
    if(sql_.inTransaction()) sql_.query("SELECT pg_advisory_xact_lock(hashtextextended($1,0))",{"tree.reply:"+str(user)+":"+std::string(key)});
    auto r=sql_.query(replySelect+"WHERE r.author_id=$1 AND r.request_key=$2",{str(user),std::string(key)});
    return r.empty()?std::optional<TreeReply>{}:parseReply(r[0]);
}
TreeReply TreePostgresRepository::insertReply(TreeReply value) {
    sql_.requireTransaction(); value.id=std::stoull(sql_.query("INSERT INTO tree_replies(post_id,author_id,request_key,body,created_ms) VALUES($1,$2,$3,$4,$5) RETURNING id",{str(value.post),str(value.author),value.requestId,value.body,std::to_string(value.createdAt)})[0][0]); if(value.parent)sql_.query("INSERT INTO tree_reply_links(reply_id,parent_reply_id) VALUES($1,$2)",{str(value.id),str(value.parent)});return value;
}
std::optional<TreeReply> TreePostgresRepository::reply(Id id) const {
    auto r=sql_.query(replySelect+"WHERE r.id=$1",{str(id)});return r.empty()?std::optional<TreeReply>{}:parseReply(r[0]);
}
void TreePostgresRepository::deleteReply(Id id) {sql_.requireTransaction();sql_.query("UPDATE tree_replies SET deleted=TRUE WHERE id=$1",{str(id)});}
std::vector<TreeReply> TreePostgresRepository::replies(Id post,Id before,std::size_t limit) const {
    std::vector<TreeReply> result;
    for(auto &r:sql_.query(replySelect+"WHERE r.post_id=$1 AND NOT r.deleted AND ($2::bigint=0 OR r.id<$2) ORDER BY r.id DESC LIMIT $3",{str(post),str(before),str(limit)})) result.push_back(parseReply(r));
    return result;
}
}
