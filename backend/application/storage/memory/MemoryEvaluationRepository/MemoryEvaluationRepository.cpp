#include "backend/application/storage/memory/MemoryEvaluationRepository/MemoryEvaluationRepository.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>
namespace webserver::phase11 {
using storage_detail::orderedUsers;
using storage_detail::UserPair;
using storage_detail::MemberKey;
using storage_detail::IdempotencyKey;
ModelEvaluationSuite MemoryEvaluationRepository::createEvaluationSuite(storage_detail::MemoryState &memory, ModelEvaluationSuite suite, std::vector<ModelEvaluationCase> cases)
{
    if (suite.name.empty() || cases.empty())
        throw std::invalid_argument("invalid evaluation suite");
    suite.id = memory.nextEvaluationSuiteId++;
    for (auto &testCase : cases)
    {
        testCase.id = memory.nextEvaluationCaseId++;
        testCase.suiteId = suite.id;
    }
    memory.evaluationSuites.emplace(suite.id, suite);
    for (auto &testCase : cases)
        memory.evaluationCases.emplace(testCase.id, std::move(testCase));
    return suite;
}

std::optional<ModelEvaluationSuite> MemoryEvaluationRepository::findEvaluationSuite(const storage_detail::MemoryState &memory, ModelEvaluationSuiteId id)
{
    const auto found = memory.evaluationSuites.find(id);
    return found == memory.evaluationSuites.end()
               ? std::nullopt
               : std::optional<ModelEvaluationSuite>{found->second};
}

std::vector<ModelEvaluationCase> MemoryEvaluationRepository::evaluationCases(const storage_detail::MemoryState &memory, ModelEvaluationSuiteId suite)
{
    std::vector<ModelEvaluationCase> result;
    for (const auto &[_, testCase] : memory.evaluationCases)
        if (testCase.suiteId == suite)
            result.push_back(testCase);
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    return result;
}

ModelEvaluationRun MemoryEvaluationRepository::createEvaluationRun(storage_detail::MemoryState &memory, ModelEvaluationRun run)
{
    const auto version = memory.modelVersions.find(run.modelVersionId);
    if (!memory.evaluationSuites.contains(run.suiteId) ||
        version == memory.modelVersions.end() ||
        run.status != ModelEvaluationRunStatus::Running || run.totalCases == 0)
        throw std::invalid_argument("invalid evaluation run");
    run.id = memory.nextEvaluationRunId++;
    memory.evaluationRuns.emplace(run.id, run);
    return run;
}

std::optional<ModelEvaluationRun> MemoryEvaluationRepository::findEvaluationRun(const storage_detail::MemoryState &memory, ModelEvaluationRunId id)
{
    const auto found = memory.evaluationRuns.find(id);
    return found == memory.evaluationRuns.end()
               ? std::nullopt
               : std::optional<ModelEvaluationRun>{found->second};
}

bool MemoryEvaluationRepository::appendEvaluationCaseResult(storage_detail::MemoryState &memory, ModelEvaluationCaseResult result)
{
    const auto run = memory.evaluationRuns.find(result.runId);
    const auto testCase = memory.evaluationCases.find(result.caseId);
    if (run == memory.evaluationRuns.end() ||
        run->second.status != ModelEvaluationRunStatus::Running ||
        testCase == memory.evaluationCases.end() ||
        testCase->second.suiteId != run->second.suiteId)
        return false;
    return memory.evaluationResults.emplace(
        std::make_pair(result.runId, result.caseId), std::move(result)).second;
}

std::vector<ModelEvaluationCaseResult> MemoryEvaluationRepository::evaluationCaseResults(const storage_detail::MemoryState &memory, ModelEvaluationRunId run)
{
    std::vector<ModelEvaluationCaseResult> result;
    for (const auto &[key, value] : memory.evaluationResults)
        if (key.first == run)
            result.push_back(value);
    return result;
}

bool MemoryEvaluationRepository::finishEvaluationRun(storage_detail::MemoryState &memory, ModelEvaluationRun run)
{
    const auto found = memory.evaluationRuns.find(run.id);
    if (found == memory.evaluationRuns.end() ||
        found->second.status != ModelEvaluationRunStatus::Running ||
        run.status == ModelEvaluationRunStatus::Running ||
        run.suiteId != found->second.suiteId ||
        run.modelVersionId != found->second.modelVersionId ||
        run.totalCases != found->second.totalCases || !run.completedAt)
        return false;
    found->second = std::move(run);
    return true;
}

bool MemoryEvaluationRepository::upsertActivationPolicy(storage_detail::MemoryState &memory, ModelActivationPolicy policy)
{
    if (!memory.modelNodes.contains(policy.nodeId) ||
        !memory.evaluationSuites.contains(policy.suiteId))
        return false;
    memory.activationPolicies.insert_or_assign(policy.nodeId, std::move(policy));
    return true;
}

std::optional<ModelActivationPolicy> MemoryEvaluationRepository::findActivationPolicy(const storage_detail::MemoryState &memory, ModelNodeId node)
{
    const auto found = memory.activationPolicies.find(node);
    return found == memory.activationPolicies.end()
               ? std::nullopt
               : std::optional<ModelActivationPolicy>{found->second};
}

std::optional<ModelEvaluationRun> MemoryEvaluationRepository::latestCompletedEvaluationRun(const storage_detail::MemoryState &memory, ModelVersionId version, ModelEvaluationSuiteId suite)
{
    std::optional<ModelEvaluationRun> latest;
    for (const auto &[_, run] : memory.evaluationRuns)
    {
        if (run.modelVersionId != version || run.suiteId != suite ||
            run.status != ModelEvaluationRunStatus::Completed ||
            !run.completedAt)
            continue;
        if (!latest || *run.completedAt > *latest->completedAt ||
            (*run.completedAt == *latest->completedAt && run.id > latest->id))
            latest = run;
    }
    return latest;
}

}
