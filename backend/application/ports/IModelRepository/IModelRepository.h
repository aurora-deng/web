#pragma once
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include "backend/application/domain/ModelTypes/ModelTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace webserver::phase11 {
class IModelRepository
{
public:
    virtual ~IModelRepository() = default;
    virtual ModelNode createModelNode(ModelNode node) = 0;
    virtual ModelVersion createModelVersion(ModelVersion version) = 0;
    virtual std::pair<ModelVersion, ModelTrainingLineage>
    createTrainedModelVersion(ModelVersion version,
                              ModelTrainingLineage lineage) = 0;
    virtual ModelEdge createModelEdge(ModelEdge edge) = 0;
    virtual bool bindAiAccount(ModelBinding binding) = 0;
    [[nodiscard]] virtual std::optional<ModelNode> findModelNode(ModelNodeId id) const = 0;
    [[nodiscard]] virtual std::vector<ModelNode> modelNodes(ModelNodeId after,std::size_t limit) const = 0;
    [[nodiscard]] virtual std::vector<ModelVersion> modelVersions(ModelNodeId node,ModelVersionId after,std::size_t limit) const = 0;
    [[nodiscard]] virtual std::optional<ModelVersion> findModelVersion(
        ModelVersionId id) const = 0;
    [[nodiscard]] virtual std::optional<ModelTrainingLineage>
    findModelTrainingLineage(ModelVersionId version) const = 0;
    [[nodiscard]] virtual std::optional<ModelBinding> findModelBinding(
        UserId aiUser) const = 0;
    [[nodiscard]] virtual std::vector<ModelEdge> modelEdges() const = 0;
    /** 成功切换和 succeeded 审计必须在同一个存储原子操作内完成。 */
    virtual bool activateModelVersion(ModelNodeId node, ModelVersionId version,
                                      ModelActivationAudit audit) = 0;
    virtual ModelActivationAudit appendModelActivationAudit(
        ModelActivationAudit audit) = 0;
    [[nodiscard]] virtual std::vector<ModelActivationAudit>
    modelActivationAudits(ModelNodeId node, std::size_t limit) const = 0;
};
}
