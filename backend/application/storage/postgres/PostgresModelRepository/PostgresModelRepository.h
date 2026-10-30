#pragma once
#include "backend/application/domain/ModelTypes/ModelTypes.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
#include "backend/application/storage/postgres/PostgresOutboxWriter/PostgresOutboxWriter.h"
namespace webserver::phase11 {
/** Model SQL adapter. Uses the caller's connection; never borrows another lease. */
class PostgresModelRepository final {
public:
    static ModelNode createModelNode(PGconn *connection, ModelNode node);
    static ModelVersion createModelVersion(PGconn *connection, ModelVersion version);
    static std::pair<ModelVersion, ModelTrainingLineage> createTrainedModelVersion(PGconn *connection, bool ownsTransaction, ModelVersion version, ModelTrainingLineage lineage);
    static ModelEdge createModelEdge(PGconn *connection, ModelEdge edge);
    static bool bindAiAccount(PGconn *connection, ModelBinding binding);
    static std::optional<ModelNode> findModelNode(PGconn *connection, ModelNodeId id);
    static std::vector<ModelNode> modelNodes(PGconn *connection, ModelNodeId after, std::size_t limit);
    static std::vector<ModelVersion> modelVersions(PGconn *connection, ModelNodeId node, ModelVersionId after, std::size_t limit);
    static std::optional<ModelVersion> findModelVersion(PGconn *connection, ModelVersionId id);
    static std::optional<ModelTrainingLineage> findModelTrainingLineage(PGconn *connection, ModelVersionId version);
    static std::optional<ModelBinding> findModelBinding(PGconn *connection, UserId aiUser);
    static std::vector<ModelEdge> modelEdges(PGconn *connection);
    static bool activateModelVersion(PGconn *connection, ModelNodeId node, ModelVersionId version, ModelActivationAudit audit);
    static ModelActivationAudit appendModelActivationAudit(PGconn *connection, ModelActivationAudit audit);
    static std::vector<ModelActivationAudit> modelActivationAudits(PGconn *connection, ModelNodeId node, std::size_t limit);
};
}
