#pragma once
#include "client/storage/SqliteStore/SqliteStore.h"
#include <map>
#include <set>
namespace nebula::client {
struct LocalDocument{std::string id,title,source,checksum;bool enabled{},authorized{};};
struct LocalCitation{std::string documentId,chunkId,title,text;double score{};};
/** SQLite 词法倒排索引；中文使用字符/相邻双字，不加载向量模型。 */
class LocalKnowledge final {
public:
    explicit LocalKnowledge(SqliteStore&store);
    LocalDocument importText(std::string title,std::string source,std::string body,bool authorized);
    std::vector<LocalDocument> documents();
    void configure(const std::string&id,bool enabled,bool authorized);
    void remove(const std::string&id);
    std::vector<LocalCitation> search(const std::string&query,std::size_t limit=6);
    static std::map<std::string,int> terms(std::string_view text);
private:SqliteStore&store_;
};
}
