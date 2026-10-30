#include "backend/application/storage/memory/MemoryModelRepository/MemoryModelRepository.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>
namespace webserver::phase11 {
using storage_detail::orderedUsers;
using storage_detail::UserPair;
using storage_detail::MemberKey;
using storage_detail::IdempotencyKey;
ModelNode MemoryModelRepository::createModelNode(storage_detail::MemoryState &memory, ModelNode node)
{
    node.id = memory.nextModelNodeId++;
    memory.modelNodes.emplace(node.id, node);
    return node;
}

ModelVersion MemoryModelRepository::createModelVersion(storage_detail::MemoryState &memory, ModelVersion version)
{
    if (!memory.modelNodes.contains(version.nodeId))
        throw std::invalid_argument("model node does not exist");
    version.id = memory.nextModelVersionId++;
    memory.modelVersions.emplace(version.id, version);
    return version;
}

std::pair<ModelVersion, ModelTrainingLineage> MemoryModelRepository::createTrainedModelVersion(storage_detail::MemoryState &memory, ModelVersion version, ModelTrainingLineage lineage)
{
    if (!memory.modelNodes.contains(version.nodeId) ||
        version.state != ModelVersionState::Candidate ||
        lineage.modelVersionId != 0 || lineage.actor.empty())
        throw std::invalid_argument("invalid trained model version");
    const bool databaseVerified =
        lineage.authorizationMode == TrainingAuthorizationMode::DatabaseVerified;
    if (databaseVerified != lineage.datasetVersionId.has_value())
        throw std::invalid_argument("invalid training authorization lineage");
    if (lineage.datasetVersionId)
    {
        const auto dataset = memory.datasets.find(*lineage.datasetVersionId);
        if (dataset == memory.datasets.end())
            throw std::invalid_argument("training dataset does not exist");
        auto checksum = dataset->second.checksum;
        if (checksum.starts_with("sha256:")) checksum.erase(0, 7);
        if (checksum != lineage.datasetSha256 ||
            dataset->second.recordCount != lineage.datasetRecords)
            throw std::invalid_argument("training dataset lineage mismatch");
    }
    version.id = memory.nextModelVersionId++;
    lineage.modelVersionId = version.id;
    memory.modelVersions.emplace(version.id, version);
    memory.modelTrainingLineages.emplace(version.id, lineage);
    return {version, lineage};
}

ModelEdge MemoryModelRepository::createModelEdge(storage_detail::MemoryState &memory, ModelEdge edge)
{
    if (edge.from == edge.to || !memory.modelNodes.contains(edge.from) ||
        !memory.modelNodes.contains(edge.to))
        throw std::invalid_argument("invalid model edge");
    edge.id = memory.nextModelEdgeId++;
    memory.modelEdges.emplace(edge.id, edge);
    return edge;
}

bool MemoryModelRepository::bindAiAccount(storage_detail::MemoryState &memory, ModelBinding binding)
{
    const auto user = memory.users.find(binding.aiUserId);
    if (user == memory.users.end() || !user->second.aiAccount ||
        !memory.modelNodes.contains(binding.nodeId))
        return false;
    memory.modelBindings.insert_or_assign(binding.aiUserId, binding);
    return true;
}

std::optional<ModelNode> MemoryModelRepository::findModelNode(const storage_detail::MemoryState &memory, ModelNodeId id)
{
    const auto found = memory.modelNodes.find(id);
    return found == memory.modelNodes.end()
               ? std::nullopt
               : std::optional<ModelNode>{found->second};
}

std::vector<ModelNode> MemoryModelRepository::modelNodes(const storage_detail::MemoryState &memory, ModelNodeId after, std::size_t limit)
{std::vector<ModelNode>result;
    for(const auto&[id,node]:memory.modelNodes)if(id>after)result.push_back(node);
    std::sort(result.begin(),result.end(),[](const auto&a,const auto&b){return a.id<b.id;});
    if(result.size()>std::min<std::size_t>(limit,50))result.resize(std::min<std::size_t>(limit,50));return result;
}

std::vector<ModelVersion> MemoryModelRepository::modelVersions(const storage_detail::MemoryState &memory, ModelNodeId node, ModelVersionId after, std::size_t limit)
{std::vector<ModelVersion>result;
    for(const auto&[id,version]:memory.modelVersions)if(id>after&&version.nodeId==node)result.push_back(version);
    std::sort(result.begin(),result.end(),[](const auto&a,const auto&b){return a.id<b.id;});
    if(result.size()>std::min<std::size_t>(limit,50))result.resize(std::min<std::size_t>(limit,50));return result;
}

std::optional<ModelVersion> MemoryModelRepository::findModelVersion(const storage_detail::MemoryState &memory, ModelVersionId id)
{
    const auto found = memory.modelVersions.find(id);
    return found == memory.modelVersions.end()
               ? std::nullopt
               : std::optional<ModelVersion>{found->second};
}

std::optional<ModelTrainingLineage> MemoryModelRepository::findModelTrainingLineage(const storage_detail::MemoryState &memory, ModelVersionId version)
{
    const auto found = memory.modelTrainingLineages.find(version);
    return found == memory.modelTrainingLineages.end()
               ? std::nullopt
               : std::optional<ModelTrainingLineage>{found->second};
}

std::optional<ModelBinding> MemoryModelRepository::findModelBinding(const storage_detail::MemoryState &memory, UserId aiUser)
{
    const auto found = memory.modelBindings.find(aiUser);
    return found == memory.modelBindings.end()
               ? std::nullopt
               : std::optional<ModelBinding>{found->second};
}

std::vector<ModelEdge> MemoryModelRepository::modelEdges(const storage_detail::MemoryState &memory)
{
    std::vector<ModelEdge> result;
    result.reserve(memory.modelEdges.size());
    for (const auto &[_, edge] : memory.modelEdges)
        result.push_back(edge);
    return result;
}

bool MemoryModelRepository::activateModelVersion(storage_detail::MemoryState &memory, ModelNodeId node, ModelVersionId version, ModelActivationAudit audit)
{
    const auto foundNode = memory.modelNodes.find(node);
    const auto foundVersion = memory.modelVersions.find(version);
    if (foundNode == memory.modelNodes.end() ||
        foundVersion == memory.modelVersions.end() ||
        foundVersion->second.nodeId != node ||
        foundVersion->second.checksum.empty() ||
        foundVersion->second.state == ModelVersionState::Rejected ||
        audit.nodeId != node || audit.targetVersionId != version ||
        audit.outcome != ModelActivationOutcome::Succeeded ||
        audit.actor.empty())
        return false;
    audit.previousVersionId = foundNode->second.activeVersionId;
    if (foundNode->second.activeVersionId)
    {
        auto &previous = memory.modelVersions.at(*foundNode->second.activeVersionId);
        previous.state = ModelVersionState::Retired;
    }
    foundVersion->second.state = ModelVersionState::Active;
    foundNode->second.activeVersionId = version;
    audit.id = memory.nextModelActivationAuditId++;
    memory.modelActivationAudits.emplace(audit.id, std::move(audit));
    return true;
}

ModelActivationAudit MemoryModelRepository::appendModelActivationAudit(storage_detail::MemoryState &memory, ModelActivationAudit audit)
{
    const auto version = memory.modelVersions.find(audit.targetVersionId);
    if (!memory.modelNodes.contains(audit.nodeId) ||
        version == memory.modelVersions.end() ||
        version->second.nodeId != audit.nodeId || audit.actor.empty())
        throw std::invalid_argument("invalid model activation audit");
    audit.id = memory.nextModelActivationAuditId++;
    memory.modelActivationAudits.emplace(audit.id, audit);
    return audit;
}

std::vector<ModelActivationAudit> MemoryModelRepository::modelActivationAudits(const storage_detail::MemoryState &memory, ModelNodeId node, std::size_t limit)
{
    std::vector<ModelActivationAudit> result;
    for (const auto &[_, audit] : memory.modelActivationAudits)
        if (audit.nodeId == node)
            result.push_back(audit);
    std::sort(result.begin(), result.end(),
              [](const auto &left, const auto &right)
              {
                  return left.createdAt != right.createdAt
                             ? left.createdAt > right.createdAt
                             : left.id > right.id;
              });
    if (result.size() > limit)
        result.resize(limit);
    return result;
}

}
