#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"

namespace webserver::phase11 {
/** EvaluationTypes: protocol/database independent business facts. */
struct ModelEvaluationSuite
{
    ModelEvaluationSuiteId id = 0;
    std::string name;
    std::string version;
    std::string checksum;
    std::string description;
    TimePoint createdAt{};
};

struct ModelEvaluationCase
{
    ModelEvaluationCaseId id = 0;
    ModelEvaluationSuiteId suiteId = 0;
    std::string name;
    std::string prompt;
    std::string expectedAnswer;
    double weight = 1.0;
    bool required = true;
};

enum class ModelEvaluationRunStatus
{
    Running,
    Completed,
    Aborted
};

struct ModelEvaluationRun
{
    ModelEvaluationRunId id = 0;
    ModelEvaluationSuiteId suiteId = 0;
    ModelVersionId modelVersionId = 0;
    ModelEvaluationRunStatus status = ModelEvaluationRunStatus::Running;
    std::uint64_t totalCases = 0;
    std::uint64_t evaluatedCases = 0;
    std::uint64_t passedCases = 0;
    double weightedScore = 0.0;
    bool requiredCasesPassed = false;
    std::string detail;
    TimePoint startedAt{};
    std::optional<TimePoint> completedAt;
};

struct ModelEvaluationCaseResult
{
    ModelEvaluationRunId runId = 0;
    ModelEvaluationCaseId caseId = 0;
    double score = 0.0;
    bool passed = false;
    std::string outputSha256;
    std::uint64_t latencyMs = 0;
    std::string detail;
};

struct ModelActivationPolicy
{
    ModelNodeId nodeId = 0;
    ModelEvaluationSuiteId suiteId = 0;
    double minimumPassRate = 1.0;
    double minimumWeightedScore = 0.0;
    std::uint64_t maxRunAgeSeconds = 86'400;
    TimePoint updatedAt{};
};

struct ModelQualityGateDecision
{
    bool allowed = false;
    std::optional<ModelEvaluationRunId> runId;
    std::string detail;
};
}
