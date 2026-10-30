#pragma once
#include "backend/application/storage/memory/MemoryState/MemoryState.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
namespace webserver::phase11 {
/** Evaluation in-memory adapter. Facade holds the shared lock during calls. */
class MemoryEvaluationRepository final {
public:
    static ModelEvaluationSuite createEvaluationSuite(storage_detail::MemoryState &memory, ModelEvaluationSuite suite, std::vector<ModelEvaluationCase> cases);
    static std::optional<ModelEvaluationSuite> findEvaluationSuite(const storage_detail::MemoryState &memory, ModelEvaluationSuiteId id);
    static std::vector<ModelEvaluationCase> evaluationCases(const storage_detail::MemoryState &memory, ModelEvaluationSuiteId suite);
    static ModelEvaluationRun createEvaluationRun(storage_detail::MemoryState &memory, ModelEvaluationRun run);
    static std::optional<ModelEvaluationRun> findEvaluationRun(const storage_detail::MemoryState &memory, ModelEvaluationRunId id);
    static bool appendEvaluationCaseResult(storage_detail::MemoryState &memory, ModelEvaluationCaseResult result);
    static std::vector<ModelEvaluationCaseResult> evaluationCaseResults(const storage_detail::MemoryState &memory, ModelEvaluationRunId run);
    static bool finishEvaluationRun(storage_detail::MemoryState &memory, ModelEvaluationRun run);
    static bool upsertActivationPolicy(storage_detail::MemoryState &memory, ModelActivationPolicy policy);
    static std::optional<ModelActivationPolicy> findActivationPolicy(const storage_detail::MemoryState &memory, ModelNodeId node);
    static std::optional<ModelEvaluationRun> latestCompletedEvaluationRun(const storage_detail::MemoryState &memory, ModelVersionId version, ModelEvaluationSuiteId suite);
};
}
