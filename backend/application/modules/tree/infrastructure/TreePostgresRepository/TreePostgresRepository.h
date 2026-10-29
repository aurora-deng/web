#pragma once
#include "backend/application/modules/tree/ports/ITreeRepository/ITreeRepository.h"
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
#include "backend/application/modules/activities/ports/IContentAudience/IContentAudience.h"

namespace webserver::function3 {
class TreePostgresRepository final : public ITreeRepository, public IContentAudience {
public:
    explicit TreePostgresRepository(SqlContext &sql) : sql_(sql) {}
    bool friendsCanView(Id author,Id viewer)const override;
    bool categoryActive(Id id) const override;
    std::vector<TreeCategory> categories() const override;
    std::vector<TreeCategory>allCategories()const override;
    TreeCategory createCategory(std::string name)override;
    bool updateCategory(Id category,std::string name,bool active)override;
    std::optional<TreePost> post(Id id,Id viewer) const override;
    std::optional<TreePost> postByKey(Id author,std::string_view key) const override;
    TreePost insertPost(TreePost post) override;
    void savePost(const TreePost &post) override;
    std::vector<TreePost> posts(const PostQuery &query,Id viewer) const override;
    std::uint64_t publishedCount() const override;
    bool setLike(Id post,Id user,bool liked) override;
    std::optional<TreeReply> replyByKey(Id author,std::string_view key) const override;
    TreeReply insertReply(TreeReply reply) override;
    std::optional<TreeReply> reply(Id id) const override;
    void deleteReply(Id id) override;
    std::vector<TreeReply> replies(Id post,Id before,std::size_t limit) const override;
private:
    std::vector<TreePost> hydrate(const SqlRows &rows) const;
    SqlContext &sql_;
};
}
