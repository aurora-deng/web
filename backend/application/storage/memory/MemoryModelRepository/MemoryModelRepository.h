#pragma once
#include "backend/application/storage/memory/MemoryState/MemoryState.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
namespace webserver::phase11 {
/** Model in-memory adapter. Facade holds the shared lock during calls. */
class MemoryModelRepository final {
public:
    static ModelNode createModelNode(storage_detail::MemoryState &memory, ModelNode node);
    static ModelVersion createModelVersion(storage_detail::MemoryState &memory, ModelVersion version);
    static std::pair<ModelVersion, ModelTrainingLineage> createTrainedModelVersion(storage_detail::MemoryState &memory, ModelVersion version, ModelTrainingLineage lineage);
    static ModelEdge createModelEdge(storage_detail::MemoryState &memory, ModelEdge edge);
    static bool bindAiAccount(storage_detail::MemoryState &memory, ModelBinding binding);
    static std::optional<ModelNode> findModelNode(const storage_detail::MemoryState &memory, ModelNodeId id);
    static std::vector<ModelNode> modelNodes(const storage_detail::MemoryState &memory, ModelNodeId after, std::size_t limit);
    static std::vector<ModelVersion> modelVersions(const storage_detail::MemoryState &memory, ModelNodeId node, ModelVersionId after, std::size_t limit);
    static std::optional<ModelVersion> findModelVersion(const storage_detail::MemoryState &memory, ModelVersionId id);
    static std::optional<ModelTrainingLineage> findModelTrainingLineage(const storage_detail::MemoryState &memory, ModelVersionId version);
    static std::optional<ModelBinding> findModelBinding(const storage_detail::MemoryState &memory, UserId aiUser);
    static std::vector<ModelEdge> modelEdges(const storage_detail::MemoryState &memory);
    static bool activateModelVersion(storage_detail::MemoryState &memory, ModelNodeId node, ModelVersionId version, ModelActivationAudit audit);
    static ModelActivationAudit appendModelActivationAudit(storage_detail::MemoryState &memory, ModelActivationAudit audit);
    static std::vector<ModelActivationAudit> modelActivationAudits(const storage_detail::MemoryState &memory, ModelNodeId node, std::size_t limit);
};
}
