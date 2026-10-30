#pragma once
#include "backend/application/domain/EvaluationTypes/EvaluationTypes.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
#include "backend/application/storage/postgres/PostgresOutboxWriter/PostgresOutboxWriter.h"
namespace webserver::phase11 {
/** Evaluation SQL adapter. Uses the caller's connection; never borrows another lease. */
class PostgresEvaluationRepository final {
public:
    static ModelEvaluationSuite createEvaluationSuite(PGconn *connection, bool ownsTransaction, ModelEvaluationSuite suite, std::vector<ModelEvaluationCase> cases);
    static std::optional<ModelEvaluationSuite> findEvaluationSuite(PGconn *connection, ModelEvaluationSuiteId id);
    static std::vector<ModelEvaluationCase> evaluationCases(PGconn *connection, ModelEvaluationSuiteId suite);
    static ModelEvaluationRun createEvaluationRun(PGconn *connection, ModelEvaluationRun run);
    static std::optional<ModelEvaluationRun> findEvaluationRun(PGconn *connection, ModelEvaluationRunId id);
    static bool appendEvaluationCaseResult(PGconn *connection, ModelEvaluationCaseResult result);
    static std::vector<ModelEvaluationCaseResult> evaluationCaseResults(PGconn *connection, ModelEvaluationRunId run);
    static bool finishEvaluationRun(PGconn *connection, ModelEvaluationRun run);
    static bool upsertActivationPolicy(PGconn *connection, ModelActivationPolicy policy);
    static std::optional<ModelActivationPolicy> findActivationPolicy(PGconn *connection, ModelNodeId node);
    static std::optional<ModelEvaluationRun> latestCompletedEvaluationRun(PGconn *connection, ModelVersionId version, ModelEvaluationSuiteId suite);
};
}
