#pragma once

#include "server/phase11/ports/Ports/Ports.h"

#include <shared_mutex>
#include <unordered_map>

namespace webserver::phase11
{

/**
 * 无外部依赖的学习型词法索引。它不是生产向量数据库，但完整实现了 Index 端口，
 * 可用于验证权限、召回、排序和引用链，未来替换设备时业务层无需变化。
 */
class DeterministicKnowledgeIndex final : public IKnowledgeIndex
{
public:
    void replaceAll(std::vector<KnowledgeChunk> chunks) override;
    void upsert(std::vector<KnowledgeChunk> chunks) override;
    void removeDocument(KnowledgeDocumentId document) override;
    [[nodiscard]] std::vector<KnowledgeSearchHit> search(
        std::string_view query,
        const std::vector<KnowledgeDocumentId> &allowedDocuments,
        std::size_t limit) const override;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<KnowledgeChunkId, KnowledgeChunk> chunks_;
};

} // namespace webserver::phase11
