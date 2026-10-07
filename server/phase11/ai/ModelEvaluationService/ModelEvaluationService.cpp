#include "server/phase11/ai/ModelEvaluationService/ModelEvaluationService.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace webserver::phase11
{
namespace
{

void requireText(const std::string &value, std::size_t maximum,
                 const char *message)
{
    if (value.empty() || value.size() > maximum)
        throw ApplicationError(ErrorCode::InvalidArgument, message);
}

} // namespace

bool ModelEvaluationService::validSha256(std::string_view value) noexcept
{
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character)
           {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f') ||
                      (character >= 'A' && character <= 'F');
           });
}

ModelEvaluationSuite ModelEvaluationService::createSuite(
    std::string name, std::string version, std::string checksum,
    std::string description,
    std::vector<ModelEvaluationCase> cases, TimePoint now)
{
    requireText(name, 120, "invalid evaluation suite name");
    requireText(version, 64, "invalid evaluation suite version");
    if (!validSha256(checksum) || description.size() > 1'000 ||
        cases.empty() || cases.size() > 256)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid evaluation suite contents");
    for (auto &testCase : cases)
    {
        requireText(testCase.name, 120, "invalid evaluation case name");
        requireText(testCase.prompt, 32'768, "invalid evaluation prompt");
        if (testCase.expectedAnswer.size() > 32'768 ||
            !std::isfinite(testCase.weight) || testCase.weight <= 0.0 ||
            testCase.weight > 1'000.0)
            throw ApplicationError(ErrorCode::InvalidArgument,
                                   "invalid evaluation case definition");
        testCase.id = 0;
        testCase.suiteId = 0;
    }
    return evaluations_.createEvaluationSuite(
        {0, std::move(name), std::move(version), std::move(checksum),
         std::move(description), now}, std::move(cases));
}

ModelEvaluationRun ModelEvaluationService::startRun(
    ModelVersionId version, ModelEvaluationSuiteId suite, TimePoint now)
{
    if (!models_.findModelVersion(version))
        throw ApplicationError(ErrorCode::NotFound, "model version not found");
    if (!evaluations_.findEvaluationSuite(suite))
        throw ApplicationError(ErrorCode::NotFound, "evaluation suite not found");
    const auto cases = evaluations_.evaluationCases(suite);
    if (cases.empty())
        throw ApplicationError(ErrorCode::Conflict,
                               "evaluation suite has no cases");
    return evaluations_.createEvaluationRun(
        {0, suite, version, ModelEvaluationRunStatus::Running,
         cases.size(), 0, 0, 0.0, false, {}, now, {}});
}

void ModelEvaluationService::recordResult(
    ModelEvaluationRunId run, ModelEvaluationCaseId testCase, double score,
    bool passed, std::string outputSha256, std::uint64_t latencyMs,
    std::string detail)
{
    const auto current = evaluations_.findEvaluationRun(run);
    if (!current)
        throw ApplicationError(ErrorCode::NotFound, "evaluation run not found");
    if (current->status != ModelEvaluationRunStatus::Running)
        throw ApplicationError(ErrorCode::Conflict,
                               "evaluation run is already terminal");
    const auto cases = evaluations_.evaluationCases(current->suiteId);
    if (std::none_of(cases.begin(), cases.end(), [&](const auto &candidate)
        { return candidate.id == testCase; }))
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "evaluation case does not belong to this run");
    if (!std::isfinite(score) || score < 0.0 || score > 1.0 ||
        (!outputSha256.empty() && !validSha256(outputSha256)) ||
        detail.size() > 1'000)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid evaluation case result");
    if (!evaluations_.appendEvaluationCaseResult(
            {run, testCase, score, passed, std::move(outputSha256),
             latencyMs, std::move(detail)}))
        throw ApplicationError(ErrorCode::Conflict,
                               "evaluation case result already exists");
}

ModelEvaluationRun ModelEvaluationService::completeRun(
    ModelEvaluationRunId run, TimePoint now)
{
    auto current = evaluations_.findEvaluationRun(run);
    if (!current)
        throw ApplicationError(ErrorCode::NotFound, "evaluation run not found");
    if (current->status != ModelEvaluationRunStatus::Running)
        throw ApplicationError(ErrorCode::Conflict,
                               "evaluation run is already terminal");
    const auto cases = evaluations_.evaluationCases(current->suiteId);
    const auto results = evaluations_.evaluationCaseResults(run);
    if (cases.empty() || results.size() != cases.size())
        throw ApplicationError(ErrorCode::Conflict,
                               "evaluation run does not contain every case result");

    std::unordered_map<ModelEvaluationCaseId, const ModelEvaluationCaseResult *>
        byCase;
    for (const auto &result : results)
        byCase.emplace(result.caseId, &result);
    double weightedTotal = 0.0;
    double totalWeight = 0.0;
    std::uint64_t passed = 0;
    bool requiredPassed = true;
    for (const auto &testCase : cases)
    {
        const auto found = byCase.find(testCase.id);
        if (found == byCase.end())
            throw ApplicationError(ErrorCode::Conflict,
                                   "evaluation result set is inconsistent");
        weightedTotal += found->second->score * testCase.weight;
        totalWeight += testCase.weight;
        if (found->second->passed)
            ++passed;
        else if (testCase.required)
            requiredPassed = false;
    }
    current->status = ModelEvaluationRunStatus::Completed;
    current->evaluatedCases = results.size();
    current->passedCases = passed;
    current->weightedScore = weightedTotal / totalWeight;
    current->requiredCasesPassed = requiredPassed;
    current->detail.clear();
    current->completedAt = now;
    if (!evaluations_.finishEvaluationRun(*current))
        throw ApplicationError(ErrorCode::Conflict,
                               "evaluation run was completed concurrently");
    return *current;
}

ModelEvaluationRun ModelEvaluationService::abortRun(
    ModelEvaluationRunId run, std::string detail, TimePoint now)
{
    auto current = evaluations_.findEvaluationRun(run);
    if (!current)
        throw ApplicationError(ErrorCode::NotFound, "evaluation run not found");
    if (current->status != ModelEvaluationRunStatus::Running)
        throw ApplicationError(ErrorCode::Conflict,
                               "evaluation run cannot be aborted");
    if (detail.empty() || detail.size() > 1'000)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid evaluation abort detail");
    current->status = ModelEvaluationRunStatus::Aborted;
    current->detail = std::move(detail);
    current->completedAt = now;
    if (!evaluations_.finishEvaluationRun(*current))
        throw ApplicationError(ErrorCode::Conflict,
                               "evaluation run was completed concurrently");
    return *current;
}

ModelActivationPolicy ModelEvaluationService::configurePolicy(
    ModelNodeId node, ModelEvaluationSuiteId suite, double minimumPassRate,
    double minimumWeightedScore, std::uint64_t maxRunAgeSeconds, TimePoint now)
{
    if (!models_.findModelNode(node))
        throw ApplicationError(ErrorCode::NotFound, "model node not found");
    if (!evaluations_.findEvaluationSuite(suite))
        throw ApplicationError(ErrorCode::NotFound, "evaluation suite not found");
    if (!std::isfinite(minimumPassRate) || minimumPassRate < 0.0 ||
        minimumPassRate > 1.0 || !std::isfinite(minimumWeightedScore) ||
        minimumWeightedScore < 0.0 || minimumWeightedScore > 1.0 ||
        maxRunAgeSeconds < 60 || maxRunAgeSeconds > 31'536'000)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid model activation policy");
    ModelActivationPolicy policy{node, suite, minimumPassRate,
                                 minimumWeightedScore, maxRunAgeSeconds, now};
    if (!evaluations_.upsertActivationPolicy(policy))
        throw ApplicationError(ErrorCode::Conflict,
                               "model activation policy could not be saved");
    return policy;
}

ModelQualityGateDecision ModelEvaluationService::assess(
    ModelNodeId node, ModelVersionId version, TimePoint now) const
{
    const auto candidate = models_.findModelVersion(version);
    if (!candidate || candidate->nodeId != node)
        return {false, {}, "model version does not belong to the node"};
    const auto policy = evaluations_.findActivationPolicy(node);
    if (!policy)
        return {true, {}, "no evaluation policy configured"};
    const auto run = evaluations_.latestCompletedEvaluationRun(
        version, policy->suiteId);
    if (!run || !run->completedAt)
        return {false, {}, "no completed evaluation run for this model version"};
    if (*run->completedAt > now ||
        now - *run->completedAt >
            std::chrono::seconds(policy->maxRunAgeSeconds))
        return {false, run->id, "the latest evaluation run is stale"};
    if (!run->requiredCasesPassed)
        return {false, run->id, "a required evaluation case failed"};
    const auto passRate = run->totalCases == 0
                              ? 0.0
                              : static_cast<double>(run->passedCases) /
                                    static_cast<double>(run->totalCases);
    if (passRate + 1e-12 < policy->minimumPassRate)
        return {false, run->id, "evaluation pass rate is below policy"};
    if (run->weightedScore + 1e-12 < policy->minimumWeightedScore)
        return {false, run->id, "evaluation weighted score is below policy"};
    return {true, run->id, "evaluation quality gate passed"};
}

} // namespace webserver::phase11
