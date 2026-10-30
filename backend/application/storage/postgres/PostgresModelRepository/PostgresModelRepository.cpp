#include "backend/application/storage/postgres/PostgresModelRepository/PostgresModelRepository.h"
#include "backend/application/storage/postgres/PostgresValueCodec/PostgresValueCodec.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace webserver::phase11 {
namespace {
using namespace storage_detail;
std::string nodeKind(ModelNodeKind kind)
{
    return kind == ModelNodeKind::Root ? "root" : "specialist";
}

ModelNodeKind nodeKind(std::string_view kind)
{
    if (kind == "root") return ModelNodeKind::Root;
    if (kind == "specialist") return ModelNodeKind::Specialist;
    throw std::runtime_error("unknown persisted model node kind");
}

std::string versionState(ModelVersionState state)
{
    switch (state)
    {
    case ModelVersionState::Candidate: return "candidate";
    case ModelVersionState::Active: return "active";
    case ModelVersionState::Retired: return "retired";
    case ModelVersionState::Rejected: return "rejected";
    }
    throw std::logic_error("unknown model version state");
}

ModelVersionState versionState(std::string_view state)
{
    if (state == "candidate") return ModelVersionState::Candidate;
    if (state == "active") return ModelVersionState::Active;
    if (state == "retired") return ModelVersionState::Retired;
    if (state == "rejected") return ModelVersionState::Rejected;
    throw std::runtime_error("unknown persisted model version state");
}

std::string trainingAuthorizationMode(TrainingAuthorizationMode mode)
{
    return mode == TrainingAuthorizationMode::DatabaseVerified
               ? "database-verified"
               : "pipeline-smoke";
}

TrainingAuthorizationMode trainingAuthorizationMode(std::string_view mode)
{
    if (mode == "pipeline-smoke")
        return TrainingAuthorizationMode::PipelineSmoke;
    if (mode == "database-verified")
        return TrainingAuthorizationMode::DatabaseVerified;
    throw std::runtime_error("unknown persisted training authorization mode");
}

std::string activationAction(ModelActivationAction action)
{
    return action == ModelActivationAction::Activate ? "activate" : "rollback";
}

ModelActivationAction activationAction(std::string_view action)
{
    if (action == "activate") return ModelActivationAction::Activate;
    if (action == "rollback") return ModelActivationAction::Rollback;
    throw std::runtime_error("unknown persisted model activation action");
}

std::string activationOutcome(ModelActivationOutcome outcome)
{
    return outcome == ModelActivationOutcome::Succeeded ? "succeeded" : "failed";
}

ModelActivationOutcome activationOutcome(std::string_view outcome)
{
    if (outcome == "succeeded") return ModelActivationOutcome::Succeeded;
    if (outcome == "failed") return ModelActivationOutcome::Failed;
    throw std::runtime_error("unknown persisted model activation outcome");
}

std::string edgeKind(ModelEdgeKind kind)
{
    return kind == ModelEdgeKind::ParentControlsChild
               ? "parent_controls_child"
               : "peer_collaborates";
}

ModelEdgeKind edgeKind(std::string_view kind)
{
    if (kind == "parent_controls_child")
        return ModelEdgeKind::ParentControlsChild;
    if (kind == "peer_collaborates")
        return ModelEdgeKind::PeerCollaborates;
    throw std::runtime_error("unknown persisted model edge kind");
}

ModelNode readNode(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)), result.value(row, 1),
            nodeKind(result.value(row, 2)), decodeOptionalId(result, row, 3),
            decodeTime(result, row, 4)};
}

ModelVersion readVersion(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)), result.value(row, 2),
            result.value(row, 3), result.value(row, 4), result.value(row, 5),
            versionState(result.value(row, 6)), decodeTime(result, row, 7)};
}

ModelTrainingLineage readTrainingLineage(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            trainingAuthorizationMode(result.value(row, 1)),
            decodeOptionalId(result, row, 2), result.value(row, 3),
            std::stoull(result.value(row, 4)), result.value(row, 5),
            result.value(row, 6), result.value(row, 7), result.value(row, 8),
            result.value(row, 9), result.value(row, 10), result.value(row, 11),
            result.value(row, 12), result.value(row, 13),
            decodeTime(result, row, 14)};
}

ModelActivationAudit readActivationAudit(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)),
            decodeOptionalId(result, row, 2),
            std::stoull(result.value(row, 3)),
            activationAction(result.value(row, 4)),
            activationOutcome(result.value(row, 5)),
            result.value(row, 6), result.value(row, 7),
            decodeTime(result, row, 8)};
}

constexpr std::string_view kNodeColumns =
    "id, name, kind, active_version_id, "
    "(extract(epoch FROM created_at) * 1000000)::bigint";

constexpr std::string_view kVersionColumns =
    "id, node_id, runtime, model_artifact, adapter_artifact, checksum, state, "
    "(extract(epoch FROM created_at) * 1000000)::bigint";

constexpr std::string_view kTrainingLineageColumns =
    "model_version_id,authorization_mode,dataset_version_id,dataset_sha256,"
    "dataset_records,base_repository,base_revision,base_weights_sha256,"
    "training_manifest_sha256,training_config_sha256,peft_weights_sha256,"
    "onnx_adapter_sha256,onnx_model_sha256,actor,"
    "(extract(epoch FROM created_at)*1000000)::bigint";

constexpr std::string_view kActivationAuditColumns =
    "id,node_id,previous_version_id,target_version_id,action,outcome,actor,detail,"
    "(extract(epoch FROM created_at) * 1000000)::bigint";
}
ModelNode PostgresModelRepository::createModelNode(PGconn *connection, ModelNode node)
{

    const auto result = query(connection,
        "INSERT INTO model_nodes(name,kind,created_at) VALUES($1,$2,to_timestamp($3::double precision/1000000.0)) RETURNING " +
        std::string(kNodeColumns), {node.name, nodeKind(node.kind), encodeTime(node.createdAt)});
    return readNode(result, 0);
}

ModelVersion PostgresModelRepository::createModelVersion(PGconn *connection, ModelVersion version)
{

    const auto result = query(connection,
        "INSERT INTO model_versions(node_id,runtime,model_artifact,adapter_artifact,checksum,state,created_at) "
        "VALUES($1::bigint,$2,$3,$4,$5,$6,to_timestamp($7::double precision/1000000.0)) RETURNING " +
        std::string(kVersionColumns),
        {number(version.nodeId), version.runtime, version.modelArtifact,
         version.adapterArtifact, version.checksum, versionState(version.state),
         encodeTime(version.createdAt)});
    return readVersion(result, 0);
}

std::pair<ModelVersion, ModelTrainingLineage> PostgresModelRepository::createTrainedModelVersion(PGconn *connection, bool ownsTransaction, ModelVersion version, ModelTrainingLineage lineage)
{

    const bool localTransaction = ownsTransaction;
    if (localTransaction) command(connection, "BEGIN");
    try
    {
        if (lineage.authorizationMode ==
            TrainingAuthorizationMode::DatabaseVerified)
        {
            if (!lineage.datasetVersionId)
                throw std::invalid_argument(
                    "database-verified lineage requires a dataset");
            const auto dataset = query(connection,
                "SELECT checksum,record_count FROM dataset_versions "
                "WHERE id=$1::bigint FOR SHARE",
                {number(*lineage.datasetVersionId)});
            if (!dataset.rows())
                throw std::invalid_argument("training dataset does not exist");
            auto checksum = dataset.value(0, 0);
            if (checksum.starts_with("sha256:")) checksum.erase(0, 7);
            if (checksum != lineage.datasetSha256 ||
                std::stoull(dataset.value(0, 1)) != lineage.datasetRecords)
                throw std::invalid_argument("training dataset lineage mismatch");
        }
        else if (lineage.datasetVersionId)
            throw std::invalid_argument(
                "pipeline-smoke lineage cannot reference a database dataset");

        const auto insertedVersion = query(connection,
            "INSERT INTO model_versions(node_id,runtime,model_artifact,"
            "adapter_artifact,checksum,state,created_at) VALUES("
            "$1::bigint,$2,$3,$4,$5,$6,"
            "to_timestamp($7::double precision/1000000.0)) RETURNING " +
            std::string(kVersionColumns),
            {number(version.nodeId), version.runtime, version.modelArtifact,
             version.adapterArtifact, version.checksum,
             versionState(version.state), encodeTime(version.createdAt)});
        version = readVersion(insertedVersion, 0);
        lineage.modelVersionId = version.id;
        const auto insertedLineage = query(connection,
            "INSERT INTO model_training_lineages("
            "model_version_id,authorization_mode,dataset_version_id,"
            "dataset_sha256,dataset_records,base_repository,base_revision,"
            "base_weights_sha256,training_manifest_sha256,"
            "training_config_sha256,peft_weights_sha256,onnx_adapter_sha256,"
            "onnx_model_sha256,actor,created_at) VALUES("
            "$1::bigint,$2,$3::bigint,$4,$5::bigint,$6,$7,$8,$9,$10,$11,$12,"
            "$13,$14,to_timestamp($15::double precision/1000000.0)) RETURNING " +
            std::string(kTrainingLineageColumns),
            {number(lineage.modelVersionId),
             trainingAuthorizationMode(lineage.authorizationMode),
             lineage.datasetVersionId
                 ? std::optional<std::string>{number(*lineage.datasetVersionId)}
                 : std::nullopt,
             lineage.datasetSha256, number(lineage.datasetRecords),
             lineage.baseRepository, lineage.baseRevision,
             lineage.baseWeightsSha256, lineage.trainingManifestSha256,
             lineage.trainingConfigSha256, lineage.peftWeightsSha256,
             lineage.onnxAdapterSha256, lineage.onnxModelSha256,
             lineage.actor, encodeTime(lineage.createdAt)});
        lineage = readTrainingLineage(insertedLineage, 0);
        if (localTransaction) command(connection, "COMMIT");
        return {version, lineage};
    }
    catch (...)
    {
        if (localTransaction)
        {
            try { command(connection, "ROLLBACK"); } catch (...) {}
        }
        throw;
    }
}

ModelEdge PostgresModelRepository::createModelEdge(PGconn *connection, ModelEdge edge)
{

    const auto result = query(connection,
        "INSERT INTO model_edges(from_node_id,to_node_id,kind) VALUES($1::bigint,$2::bigint,$3) "
        "RETURNING id,from_node_id,to_node_id,kind",
        {number(edge.from), number(edge.to), edgeKind(edge.kind)});
    return {std::stoull(result.value(0, 0)), std::stoull(result.value(0, 1)),
            std::stoull(result.value(0, 2)), edgeKind(result.value(0, 3))};
}

bool PostgresModelRepository::bindAiAccount(PGconn *connection, ModelBinding binding)
{

    const auto user = query(connection,
        "SELECT 1 FROM users WHERE id=$1::bigint AND ai_account=true",
        {number(binding.aiUserId)});
    if (!user.rows()) return false;
    return query(connection,
        "INSERT INTO model_bindings(ai_user_id,model_node_id) VALUES($1::bigint,$2::bigint) "
        "ON CONFLICT(ai_user_id) DO UPDATE SET model_node_id=EXCLUDED.model_node_id RETURNING 1",
        {number(binding.aiUserId), number(binding.nodeId)}).rows() == 1;
}

std::optional<ModelNode> PostgresModelRepository::findModelNode(PGconn *connection, ModelNodeId id)
{

    const auto result = query(connection,
        "SELECT " + std::string(kNodeColumns) + " FROM model_nodes WHERE id=$1::bigint",
        {number(id)});
    return result.rows() ? std::optional<ModelNode>{readNode(result, 0)} : std::nullopt;
}

std::vector<ModelNode> PostgresModelRepository::modelNodes(PGconn *connection, ModelNodeId after, std::size_t limit)
{
    auto rows=query(connection,"SELECT "+std::string(kNodeColumns)+" FROM model_nodes WHERE id>$1::bigint ORDER BY id LIMIT $2::bigint",{number(after),number(std::min<std::size_t>(limit,50))});
    std::vector<ModelNode>result;for(int i=0;i<rows.rows();++i)result.push_back(readNode(rows,i));return result;
}

std::vector<ModelVersion> PostgresModelRepository::modelVersions(PGconn *connection, ModelNodeId node, ModelVersionId after, std::size_t limit)
{
    auto rows=query(connection,"SELECT "+std::string(kVersionColumns)+" FROM model_versions WHERE node_id=$1::bigint AND id>$2::bigint ORDER BY id LIMIT $3::bigint",{number(node),number(after),number(std::min<std::size_t>(limit,50))});
    std::vector<ModelVersion>result;for(int i=0;i<rows.rows();++i)result.push_back(readVersion(rows,i));return result;
}

std::optional<ModelVersion> PostgresModelRepository::findModelVersion(PGconn *connection, ModelVersionId id)
{

    const auto result = query(connection,
        "SELECT " + std::string(kVersionColumns) + " FROM model_versions WHERE id=$1::bigint",
        {number(id)});
    return result.rows() ? std::optional<ModelVersion>{readVersion(result, 0)}
                         : std::nullopt;
}

std::optional<ModelTrainingLineage> PostgresModelRepository::findModelTrainingLineage(PGconn *connection, ModelVersionId version)
{

    const auto result = query(connection,
        "SELECT " + std::string(kTrainingLineageColumns) +
        " FROM model_training_lineages WHERE model_version_id=$1::bigint",
        {number(version)});
    return result.rows()
               ? std::optional<ModelTrainingLineage>{
                     readTrainingLineage(result, 0)}
               : std::nullopt;
}

std::optional<ModelBinding> PostgresModelRepository::findModelBinding(PGconn *connection, UserId aiUser)
{

    const auto result = query(connection,
        "SELECT ai_user_id,model_node_id FROM model_bindings WHERE ai_user_id=$1::bigint",
        {number(aiUser)});
    return result.rows()
               ? std::optional<ModelBinding>{{std::stoull(result.value(0, 0)),
                                               std::stoull(result.value(0, 1))}}
               : std::nullopt;
}

std::vector<ModelEdge> PostgresModelRepository::modelEdges(PGconn *connection)
{

    const auto result = query(connection,
        "SELECT id,from_node_id,to_node_id,kind FROM model_edges ORDER BY id");
    std::vector<ModelEdge> edges;
    edges.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        edges.push_back({std::stoull(result.value(row, 0)),
                         std::stoull(result.value(row, 1)),
                         std::stoull(result.value(row, 2)),
                         edgeKind(result.value(row, 3))});
    return edges;
}

bool PostgresModelRepository::activateModelVersion(PGconn *connection, ModelNodeId node, ModelVersionId version, ModelActivationAudit audit)
{
    if (audit.nodeId != node || audit.targetVersionId != version ||
        audit.outcome != ModelActivationOutcome::Succeeded ||
        audit.actor.empty())
        return false;

    const auto result = query(connection,
        "WITH locked_node AS ("
        " SELECT active_version_id FROM model_nodes WHERE id=$1::bigint FOR UPDATE"
        "), target AS ("
        " SELECT id FROM model_versions WHERE id=$2::bigint AND node_id=$1::bigint"
        " AND checksum<>'' AND state<>'rejected'"
        "), retired AS ("
        " UPDATE model_versions SET state='retired' WHERE id=(SELECT active_version_id FROM locked_node)"
        " AND id<>$2::bigint AND EXISTS(SELECT 1 FROM target)"
        "), activated AS ("
        " UPDATE model_versions SET state='active' WHERE id=(SELECT id FROM target) RETURNING id"
        "), updated AS ("
        " UPDATE model_nodes SET active_version_id=(SELECT id FROM activated)"
        " WHERE id=$1::bigint AND EXISTS(SELECT 1 FROM activated) RETURNING id"
        "), audited AS ("
        " INSERT INTO model_activation_audits("
        "node_id,previous_version_id,target_version_id,action,outcome,actor,detail,created_at)"
        " SELECT $1::bigint,(SELECT active_version_id FROM locked_node),$2::bigint,"
        "$3,$4,$5,$6,to_timestamp($7::double precision/1000000.0)"
        " WHERE EXISTS(SELECT 1 FROM updated) RETURNING id"
        ") SELECT 1 FROM audited",
        {number(node), number(version), activationAction(audit.action),
         activationOutcome(audit.outcome), audit.actor, audit.detail,
         encodeTime(audit.createdAt)});
    return result.rows() == 1;
}

ModelActivationAudit PostgresModelRepository::appendModelActivationAudit(PGconn *connection, ModelActivationAudit audit)
{

    const auto result = query(connection,
        "INSERT INTO model_activation_audits("
        "node_id,previous_version_id,target_version_id,action,outcome,actor,detail,created_at) "
        "VALUES($1::bigint,$2::bigint,$3::bigint,$4,$5,$6,$7,"
        "to_timestamp($8::double precision/1000000.0)) RETURNING " +
        std::string(kActivationAuditColumns),
        {number(audit.nodeId),
         audit.previousVersionId
             ? std::optional<std::string>{number(*audit.previousVersionId)}
             : std::nullopt,
         number(audit.targetVersionId), activationAction(audit.action),
         activationOutcome(audit.outcome), audit.actor, audit.detail,
         encodeTime(audit.createdAt)});
    return readActivationAudit(result, 0);
}

std::vector<ModelActivationAudit> PostgresModelRepository::modelActivationAudits(PGconn *connection, ModelNodeId node, std::size_t limit)
{

    const auto result = query(connection,
        "SELECT " + std::string(kActivationAuditColumns) +
        " FROM model_activation_audits WHERE node_id=$1::bigint "
        "ORDER BY created_at DESC,id DESC LIMIT $2::bigint",
        {number(node), number(limit)});
    std::vector<ModelActivationAudit> audits;
    audits.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        audits.push_back(readActivationAudit(result, row));
    return audits;
}

}
