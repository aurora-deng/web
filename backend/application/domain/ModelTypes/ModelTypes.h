#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"

namespace webserver::phase11 {
/** ModelTypes: protocol/database independent business facts. */
enum class ModelNodeKind
{
    Root,
    Specialist
};

struct ModelNode
{
    ModelNodeId id = 0;
    std::string name;
    ModelNodeKind kind = ModelNodeKind::Specialist;
    std::optional<ModelVersionId> activeVersionId;
    TimePoint createdAt{};
};

enum class ModelVersionState
{
    Candidate,
    Active,
    Retired,
    Rejected
};

struct ModelVersion
{
    ModelVersionId id = 0;
    ModelNodeId nodeId = 0;
    std::string runtime;       // ollama / onnx-inprocess / onnx-grpc
    std::string modelArtifact;
    std::string adapterArtifact;
    std::string checksum;
    ModelVersionState state = ModelVersionState::Candidate;
    TimePoint createdAt{};
};

enum class TrainingAuthorizationMode
{
    PipelineSmoke,
    DatabaseVerified
};

struct ModelTrainingLineage
{
    ModelVersionId modelVersionId = 0;
    TrainingAuthorizationMode authorizationMode =
        TrainingAuthorizationMode::PipelineSmoke;
    std::optional<DatasetVersionId> datasetVersionId;
    std::string datasetSha256;
    std::uint64_t datasetRecords = 0;
    std::string baseRepository;
    std::string baseRevision;
    std::string baseWeightsSha256;
    std::string trainingManifestSha256;
    std::string trainingConfigSha256;
    std::string peftWeightsSha256;
    std::string onnxAdapterSha256;
    std::string onnxModelSha256;
    std::string actor;
    TimePoint createdAt{};
};

enum class ModelEdgeKind
{
    ParentControlsChild,
    PeerCollaborates
};

struct ModelEdge
{
    ModelEdgeId id = 0;
    ModelNodeId from = 0;
    ModelNodeId to = 0;
    ModelEdgeKind kind = ModelEdgeKind::PeerCollaborates;
};

struct ModelBinding
{
    UserId aiUserId = 0;
    ModelNodeId nodeId = 0;
};

enum class ModelActivationAction
{
    Activate,
    Rollback
};

enum class ModelActivationOutcome
{
    Succeeded,
    Failed
};

struct ModelActivationAudit
{
    ModelActivationAuditId id = 0;
    ModelNodeId nodeId = 0;
    std::optional<ModelVersionId> previousVersionId;
    ModelVersionId targetVersionId = 0;
    ModelActivationAction action = ModelActivationAction::Activate;
    ModelActivationOutcome outcome = ModelActivationOutcome::Succeeded;
    std::string actor;
    std::string detail;
    TimePoint createdAt{};
};
}
