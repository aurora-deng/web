#include "backend/application/storage/postgres/PostgresEvaluationRepository/PostgresEvaluationRepository.h"
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
std::string evaluationRunStatus(ModelEvaluationRunStatus status)
{
    switch (status)
    {
    case ModelEvaluationRunStatus::Running: return "running";
    case ModelEvaluationRunStatus::Completed: return "completed";
    case ModelEvaluationRunStatus::Aborted: return "aborted";
    }
    throw std::logic_error("unknown model evaluation run status");
}

ModelEvaluationRunStatus evaluationRunStatus(std::string_view status)
{
    if (status == "running") return ModelEvaluationRunStatus::Running;
    if (status == "completed") return ModelEvaluationRunStatus::Completed;
    if (status == "aborted") return ModelEvaluationRunStatus::Aborted;
    throw std::runtime_error("unknown persisted model evaluation run status");
}

ModelEvaluationSuite readEvaluationSuite(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)), result.value(row, 1),
            result.value(row, 2), result.value(row, 3), result.value(row, 4),
            decodeTime(result, row, 5)};
}

ModelEvaluationCase readEvaluationCase(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)), result.value(row, 2),
            result.value(row, 3), result.value(row, 4),
            std::stod(result.value(row, 5)), decodeBool(result, row, 6)};
}

ModelEvaluationRun readEvaluationRun(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)),
            std::stoull(result.value(row, 2)),
            evaluationRunStatus(result.value(row, 3)),
            std::stoull(result.value(row, 4)),
            std::stoull(result.value(row, 5)),
            std::stoull(result.value(row, 6)),
            std::stod(result.value(row, 7)), decodeBool(result, row, 8),
            result.value(row, 9), decodeTime(result, row, 10),
            decodeOptionalTime(result, row, 11)};
}

ModelEvaluationCaseResult readEvaluationCaseResult(
    const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)),
            std::stod(result.value(row, 2)), decodeBool(result, row, 3),
            result.value(row, 4), std::stoull(result.value(row, 5)),
            result.value(row, 6)};
}

ModelActivationPolicy readActivationPolicy(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)),
            std::stod(result.value(row, 2)),
            std::stod(result.value(row, 3)),
            std::stoull(result.value(row, 4)), decodeTime(result, row, 5)};
}

constexpr std::string_view kEvaluationSuiteColumns =
    "id,name,version,checksum,description,"
    "(extract(epoch FROM created_at)*1000000)::bigint";

constexpr std::string_view kEvaluationCaseColumns =
    "id,suite_id,name,prompt,expected_answer,weight::text,required";

constexpr std::string_view kEvaluationRunColumns =
    "id,suite_id,model_version_id,status,total_cases,evaluated_cases,passed_cases,"
    "weighted_score::text,required_cases_passed,detail,"
    "(extract(epoch FROM started_at)*1000000)::bigint,"
    "(extract(epoch FROM completed_at)*1000000)::bigint";

constexpr std::string_view kEvaluationResultColumns =
    "run_id,case_id,score::text,passed,output_sha256,latency_ms,detail";

constexpr std::string_view kActivationPolicyColumns =
    "node_id,suite_id,minimum_pass_rate::text,minimum_weighted_score::text,"
    "max_run_age_seconds,(extract(epoch FROM updated_at)*1000000)::bigint";
}
ModelEvaluationSuite PostgresEvaluationRepository::createEvaluationSuite(PGconn *connection, bool ownsTransaction, ModelEvaluationSuite suite, std::vector<ModelEvaluationCase> cases)
{

    const bool localTransaction = ownsTransaction;
    if (localTransaction) command(connection, "BEGIN");
    try
    {
        const auto inserted = query(connection,
            "INSERT INTO model_evaluation_suites("
            "name,version,checksum,description,created_at) "
            "VALUES($1,$2,$3,$4,to_timestamp($5::double precision/1000000.0)) RETURNING " +
            std::string(kEvaluationSuiteColumns),
            {suite.name, suite.version, suite.checksum, suite.description,
             encodeTime(suite.createdAt)});
        suite = readEvaluationSuite(inserted, 0);
        for (auto &testCase : cases)
        {
            const auto row = query(connection,
                "INSERT INTO model_evaluation_cases("
                "suite_id,name,prompt,expected_answer,weight,required) "
                "VALUES($1::bigint,$2,$3,$4,$5::double precision,$6::boolean) RETURNING " +
                std::string(kEvaluationCaseColumns),
                {number(suite.id), testCase.name, testCase.prompt,
                 testCase.expectedAnswer, std::to_string(testCase.weight),
                 testCase.required ? "true" : "false"});
            testCase = readEvaluationCase(row, 0);
        }
        if (localTransaction) command(connection, "COMMIT");
        return suite;
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

std::optional<ModelEvaluationSuite> PostgresEvaluationRepository::findEvaluationSuite(PGconn *connection, ModelEvaluationSuiteId id)
{

    const auto result = query(connection,
        "SELECT " + std::string(kEvaluationSuiteColumns) +
        " FROM model_evaluation_suites WHERE id=$1::bigint", {number(id)});
    return result.rows()
               ? std::optional<ModelEvaluationSuite>{readEvaluationSuite(result, 0)}
               : std::nullopt;
}

std::vector<ModelEvaluationCase> PostgresEvaluationRepository::evaluationCases(PGconn *connection, ModelEvaluationSuiteId suite)
{

    const auto result = query(connection,
        "SELECT " + std::string(kEvaluationCaseColumns) +
        " FROM model_evaluation_cases WHERE suite_id=$1::bigint ORDER BY id",
        {number(suite)});
    std::vector<ModelEvaluationCase> cases;
    cases.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        cases.push_back(readEvaluationCase(result, row));
    return cases;
}

ModelEvaluationRun PostgresEvaluationRepository::createEvaluationRun(PGconn *connection, ModelEvaluationRun run)
{

    const auto result = query(connection,
        "INSERT INTO model_evaluation_runs("
        "suite_id,model_version_id,status,total_cases,evaluated_cases,passed_cases,"
        "weighted_score,required_cases_passed,detail,started_at) VALUES("
        "$1::bigint,$2::bigint,$3,$4::bigint,$5::bigint,$6::bigint,"
        "$7::double precision,$8::boolean,$9,"
        "to_timestamp($10::double precision/1000000.0)) RETURNING " +
        std::string(kEvaluationRunColumns),
        {number(run.suiteId), number(run.modelVersionId),
         evaluationRunStatus(run.status), number(run.totalCases),
         number(run.evaluatedCases), number(run.passedCases),
         std::to_string(run.weightedScore),
         run.requiredCasesPassed ? "true" : "false", run.detail,
         encodeTime(run.startedAt)});
    return readEvaluationRun(result, 0);
}

std::optional<ModelEvaluationRun> PostgresEvaluationRepository::findEvaluationRun(PGconn *connection, ModelEvaluationRunId id)
{

    const auto result = query(connection,
        "SELECT " + std::string(kEvaluationRunColumns) +
        " FROM model_evaluation_runs WHERE id=$1::bigint", {number(id)});
    return result.rows()
               ? std::optional<ModelEvaluationRun>{readEvaluationRun(result, 0)}
               : std::nullopt;
}

bool PostgresEvaluationRepository::appendEvaluationCaseResult(PGconn *connection, ModelEvaluationCaseResult result)
{

    return query(connection,
        "INSERT INTO model_evaluation_case_results("
        "run_id,case_id,score,passed,output_sha256,latency_ms,detail) "
        "SELECT r.id,c.id,$3::double precision,$4::boolean,$5,$6::bigint,$7 "
        "FROM model_evaluation_runs r "
        "JOIN model_evaluation_cases c ON c.id=$2::bigint AND c.suite_id=r.suite_id "
        "WHERE r.id=$1::bigint AND r.status='running' "
        "ON CONFLICT(run_id,case_id) DO NOTHING RETURNING " +
        std::string(kEvaluationResultColumns),
        {number(result.runId), number(result.caseId),
         std::to_string(result.score), result.passed ? "true" : "false",
         result.outputSha256, number(result.latencyMs), result.detail}).rows() == 1;
}

std::vector<ModelEvaluationCaseResult> PostgresEvaluationRepository::evaluationCaseResults(PGconn *connection, ModelEvaluationRunId run)
{

    const auto result = query(connection,
        "SELECT " + std::string(kEvaluationResultColumns) +
        " FROM model_evaluation_case_results WHERE run_id=$1::bigint ORDER BY case_id",
        {number(run)});
    std::vector<ModelEvaluationCaseResult> results;
    results.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        results.push_back(readEvaluationCaseResult(result, row));
    return results;
}

bool PostgresEvaluationRepository::finishEvaluationRun(PGconn *connection, ModelEvaluationRun run)
{

    return query(connection,
        "UPDATE model_evaluation_runs SET status=$4,evaluated_cases=$5::bigint,"
        "passed_cases=$6::bigint,weighted_score=$7::double precision,"
        "required_cases_passed=$8::boolean,detail=$9,"
        "completed_at=to_timestamp($10::double precision/1000000.0) "
        "WHERE id=$1::bigint AND suite_id=$2::bigint AND model_version_id=$3::bigint "
        "AND status='running' AND $4<>'running' "
        "AND ($4<>'completed' OR (SELECT count(*) FROM model_evaluation_case_results "
        "WHERE run_id=$1::bigint)=total_cases) RETURNING 1",
        {number(run.id), number(run.suiteId), number(run.modelVersionId),
         evaluationRunStatus(run.status), number(run.evaluatedCases),
         number(run.passedCases), std::to_string(run.weightedScore),
         run.requiredCasesPassed ? "true" : "false", run.detail,
         run.completedAt ? std::optional<std::string>{encodeTime(*run.completedAt)}
                         : std::nullopt}).rows() == 1;
}

bool PostgresEvaluationRepository::upsertActivationPolicy(PGconn *connection, ModelActivationPolicy policy)
{

    return query(connection,
        "INSERT INTO model_activation_policies("
        "node_id,suite_id,minimum_pass_rate,minimum_weighted_score,"
        "max_run_age_seconds,updated_at) VALUES("
        "$1::bigint,$2::bigint,$3::double precision,$4::double precision,"
        "$5::bigint,to_timestamp($6::double precision/1000000.0)) "
        "ON CONFLICT(node_id) DO UPDATE SET suite_id=EXCLUDED.suite_id,"
        "minimum_pass_rate=EXCLUDED.minimum_pass_rate,"
        "minimum_weighted_score=EXCLUDED.minimum_weighted_score,"
        "max_run_age_seconds=EXCLUDED.max_run_age_seconds,"
        "updated_at=EXCLUDED.updated_at RETURNING 1",
        {number(policy.nodeId), number(policy.suiteId),
         std::to_string(policy.minimumPassRate),
         std::to_string(policy.minimumWeightedScore),
         number(policy.maxRunAgeSeconds), encodeTime(policy.updatedAt)}).rows() == 1;
}

std::optional<ModelActivationPolicy> PostgresEvaluationRepository::findActivationPolicy(PGconn *connection, ModelNodeId node)
{

    const auto result = query(connection,
        "SELECT " + std::string(kActivationPolicyColumns) +
        " FROM model_activation_policies WHERE node_id=$1::bigint",
        {number(node)});
    return result.rows()
               ? std::optional<ModelActivationPolicy>{readActivationPolicy(result, 0)}
               : std::nullopt;
}

std::optional<ModelEvaluationRun> PostgresEvaluationRepository::latestCompletedEvaluationRun(PGconn *connection, ModelVersionId version, ModelEvaluationSuiteId suite)
{

    const auto result = query(connection,
        "SELECT " + std::string(kEvaluationRunColumns) +
        " FROM model_evaluation_runs WHERE model_version_id=$1::bigint "
        "AND suite_id=$2::bigint AND status='completed' "
        "ORDER BY completed_at DESC,id DESC LIMIT 1",
        {number(version), number(suite)});
    return result.rows()
               ? std::optional<ModelEvaluationRun>{readEvaluationRun(result, 0)}
               : std::nullopt;
}

}
