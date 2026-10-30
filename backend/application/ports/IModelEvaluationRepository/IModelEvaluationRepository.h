#pragma once
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include "backend/application/domain/EvaluationTypes/EvaluationTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace webserver::phase11 {
class IModelEvaluationRepository
{
public:
    virtual ~IModelEvaluationRepository() = default;
    /** 试卷和题目必须一起落库，不能留下没有题目的半成品评测集。 */
    virtual ModelEvaluationSuite createEvaluationSuite(
        ModelEvaluationSuite suite,
        std::vector<ModelEvaluationCase> cases) = 0;
    [[nodiscard]] virtual std::optional<ModelEvaluationSuite>
    findEvaluationSuite(ModelEvaluationSuiteId id) const = 0;
    [[nodiscard]] virtual std::vector<ModelEvaluationCase> evaluationCases(
        ModelEvaluationSuiteId suite) const = 0;
    virtual ModelEvaluationRun createEvaluationRun(ModelEvaluationRun run) = 0;
    [[nodiscard]] virtual std::optional<ModelEvaluationRun> findEvaluationRun(
        ModelEvaluationRunId id) const = 0;
    /** 同一运行中的同一道题只能写入一次结果。 */
    virtual bool appendEvaluationCaseResult(
        ModelEvaluationCaseResult result) = 0;
    [[nodiscard]] virtual std::vector<ModelEvaluationCaseResult>
    evaluationCaseResults(ModelEvaluationRunId run) const = 0;
    /** 只允许把 running 原子推进到 completed/aborted。 */
    virtual bool finishEvaluationRun(ModelEvaluationRun run) = 0;
    virtual bool upsertActivationPolicy(ModelActivationPolicy policy) = 0;
    [[nodiscard]] virtual std::optional<ModelActivationPolicy>
    findActivationPolicy(ModelNodeId node) const = 0;
    [[nodiscard]] virtual std::optional<ModelEvaluationRun>
    latestCompletedEvaluationRun(ModelVersionId version,
                                 ModelEvaluationSuiteId suite) const = 0;
};
}
