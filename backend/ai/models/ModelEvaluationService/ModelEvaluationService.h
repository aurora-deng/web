#pragma once

#include "backend/application/business/ApplicationError/ApplicationError.h"
#include "backend/application/ports/IModelRepository/IModelRepository.h"
#include "backend/application/ports/IModelEvaluationRepository/IModelEvaluationRepository.h"
#include "backend/application/ports/IModelQualityGate/IModelQualityGate.h"
#include "backend/application/domain/EvaluationTypes/EvaluationTypes.h"

#include <string_view>

namespace webserver::phase11
{

/**
 * 管理可复现的离线评测，并为 ModelRegistryService 提供启用门禁。
 * 生成模型输出和打分可以由独立 Worker 完成；本服务负责规则、汇总与持久化边界。
 */
class ModelEvaluationService final : public IModelQualityGate
{
public:
    ModelEvaluationService(IModelRepository &models,
                           IModelEvaluationRepository &evaluations)
        : models_(models), evaluations_(evaluations)
    {
    }

    ModelEvaluationSuite createSuite(
        std::string name, std::string version, std::string checksum,
        std::string description,
        std::vector<ModelEvaluationCase> cases, TimePoint now);
    ModelEvaluationRun startRun(ModelVersionId version,
                                ModelEvaluationSuiteId suite,
                                TimePoint now);
    void recordResult(ModelEvaluationRunId run, ModelEvaluationCaseId testCase,
                      double score, bool passed, std::string outputSha256,
                      std::uint64_t latencyMs, std::string detail = {});
    ModelEvaluationRun completeRun(ModelEvaluationRunId run, TimePoint now);
    ModelEvaluationRun abortRun(ModelEvaluationRunId run, std::string detail,
                                TimePoint now);
    ModelActivationPolicy configurePolicy(
        ModelNodeId node, ModelEvaluationSuiteId suite,
        double minimumPassRate, double minimumWeightedScore,
        std::uint64_t maxRunAgeSeconds, TimePoint now);

    [[nodiscard]] ModelQualityGateDecision assess(
        ModelNodeId node, ModelVersionId version,
        TimePoint now) const override;

private:
    static bool validSha256(std::string_view value) noexcept;

    IModelRepository &models_;
    IModelEvaluationRepository &evaluations_;
};

} // namespace webserver::phase11
