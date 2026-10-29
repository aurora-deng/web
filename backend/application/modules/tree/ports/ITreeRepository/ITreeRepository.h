#pragma once
#include "backend/application/modules/tree/domain/TreePost/TreePost.h"
#include "backend/application/modules/tree/domain/TreeReply/TreeReply.h"

namespace webserver::function3 {
class ITreeRepository {
public:
    virtual ~ITreeRepository() = default;
    virtual bool categoryActive(Id category) const = 0;
    virtual std::vector<TreeCategory> categories() const = 0;
    virtual std::vector<TreeCategory> allCategories() const = 0;
    virtual TreeCategory createCategory(std::string name)=0;
    virtual bool updateCategory(Id category,std::string name,bool active)=0;
    virtual std::optional<TreePost> post(Id id, Id viewer) const = 0;
    virtual std::optional<TreePost> postByKey(Id author, std::string_view key) const = 0;
    virtual TreePost insertPost(TreePost post) = 0;
    virtual void savePost(const TreePost &post) = 0;
    virtual std::vector<TreePost> posts(const PostQuery &query, Id viewer) const = 0;
    virtual std::uint64_t publishedCount() const = 0;
    virtual bool setLike(Id post, Id user, bool liked) = 0;
    virtual std::optional<TreeReply> replyByKey(Id author, std::string_view key) const = 0;
    virtual TreeReply insertReply(TreeReply reply) = 0;
    virtual std::optional<TreeReply> reply(Id id) const = 0;
    virtual void deleteReply(Id id) = 0;
    virtual std::vector<TreeReply> replies(Id post, Id before, std::size_t limit) const = 0;
};
/** 媒体元数据只能来自已完成校验的存储，不能相信浏览器申报的时长和类型。 */
class ITreeMediaCatalog {
public:
    virtual ~ITreeMediaCatalog() = default;
    virtual TreeMedia requireOwnedReadyMedia(Id owner, Id file) const = 0;
};
}
