#include "backend/application/storage/postgres/PostgresTrainingRepository/PostgresTrainingRepository.h"
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
std::string trainingState(TrainingCandidateState state)
{
    switch (state)
    {
    case TrainingCandidateState::Submitted: return "submitted";
    case TrainingCandidateState::Approved: return "approved";
    case TrainingCandidateState::Rejected: return "rejected";
    case TrainingCandidateState::Exported: return "exported";
    case TrainingCandidateState::Revoked: return "revoked";
    }
    throw std::logic_error("unknown training state");
}

TrainingCandidateState trainingState(std::string_view state)
{
    if (state == "submitted") return TrainingCandidateState::Submitted;
    if (state == "approved") return TrainingCandidateState::Approved;
    if (state == "rejected") return TrainingCandidateState::Rejected;
    if (state == "exported") return TrainingCandidateState::Exported;
    if (state == "revoked") return TrainingCandidateState::Revoked;
    throw std::runtime_error("unknown persisted training state");
}

TrainingCandidate readCandidate(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)),
            std::stoull(result.value(row, 2)), result.value(row, 3),
            result.value(row, 4), result.value(row, 5),
            trainingState(result.value(row, 6)),
            decodeOptionalId(result, row, 7), decodeTime(result, row, 8)};
}

DatasetVersion readDatasetVersion(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)), result.value(row, 1),
            result.value(row, 2), {}, decodeTime(result, row, 3),
            result.value(row, 4), std::stoull(result.value(row, 5))};
}

constexpr std::string_view kCandidateColumns =
    "id, owner_user_id, source_message_id, sanitized_prompt, sanitized_response, "
    "consent_version, state, dataset_version_id, "
    "(extract(epoch FROM created_at) * 1000000)::bigint";

constexpr std::string_view kDatasetVersionColumns =
    "id,name,checksum,(extract(epoch FROM created_at)*1000000)::bigint,"
    "artifact_path,record_count";
}
TrainingCandidate PostgresTrainingRepository::createTrainingCandidate(PGconn *connection, TrainingCandidate candidate)
{

    const auto result = query(connection,
        "INSERT INTO training_candidates(owner_user_id,source_message_id,sanitized_prompt,sanitized_response,consent_version,state,dataset_version_id,created_at) "
        "VALUES($1::bigint,$2::bigint,$3,$4,$5,$6,NULL,to_timestamp($7::double precision/1000000.0)) RETURNING " +
        std::string(kCandidateColumns),
        {number(candidate.ownerUserId), number(candidate.sourceMessageId),
         candidate.sanitizedPrompt, candidate.sanitizedResponse,
         candidate.consentVersion, trainingState(candidate.state),
         encodeTime(candidate.createdAt)});
    return readCandidate(result, 0);
}

std::optional<TrainingCandidate> PostgresTrainingRepository::findTrainingCandidate(PGconn *connection, TrainingCandidateId id)
{

    const auto result = query(connection,
        "SELECT " + std::string(kCandidateColumns) +
        " FROM training_candidates WHERE id=$1::bigint", {number(id)});
    return result.rows()
               ? std::optional<TrainingCandidate>{readCandidate(result, 0)}
               : std::nullopt;
}

std::optional<TrainingCandidate> PostgresTrainingRepository::findActiveTrainingCandidate(PGconn *connection, UserId owner, MessageId sourceMessage)
{

    const auto result = query(connection,
        "SELECT " + std::string(kCandidateColumns) +
        " FROM training_candidates WHERE owner_user_id=$1::bigint "
        "AND source_message_id=$2::bigint AND state<>'revoked' LIMIT 1",
        {number(owner), number(sourceMessage)});
    return result.rows()
               ? std::optional<TrainingCandidate>{readCandidate(result, 0)}
               : std::nullopt;
}

std::vector<TrainingCandidate> PostgresTrainingRepository::trainingCandidatesForOwner(PGconn *connection, UserId owner)
{

    const auto result = query(connection,
        "SELECT " + std::string(kCandidateColumns) +
        " FROM training_candidates WHERE owner_user_id=$1::bigint ORDER BY id",
        {number(owner)});
    std::vector<TrainingCandidate> candidates;
    candidates.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        candidates.push_back(readCandidate(result, row));
    return candidates;
}

std::vector<TrainingCandidate> PostgresTrainingRepository::trainingCandidatesByState(PGconn *connection, TrainingCandidateState state)
{

    const auto result = query(connection,
        "SELECT " + std::string(kCandidateColumns) +
            " FROM training_candidates WHERE state=$1 ORDER BY id",
        {trainingState(state)});
    std::vector<TrainingCandidate> candidates;
    candidates.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        candidates.push_back(readCandidate(result, row));
    return candidates;
}

bool PostgresTrainingRepository::updateTrainingCandidate(PGconn *connection, TrainingCandidate candidate)
{

    return query(connection,
        "UPDATE training_candidates SET sanitized_prompt=$2,sanitized_response=$3,consent_version=$4,state=$5,dataset_version_id=$6::bigint "
        "WHERE id=$1::bigint RETURNING 1",
        {number(candidate.id), candidate.sanitizedPrompt, candidate.sanitizedResponse,
         candidate.consentVersion, trainingState(candidate.state),
         candidate.datasetVersionId ? std::optional<std::string>{number(*candidate.datasetVersionId)} : std::nullopt}).rows() == 1;
}

bool PostgresTrainingRepository::transitionTrainingCandidate(PGconn *connection, TrainingCandidateId id, TrainingCandidateState expected, TrainingCandidateState next, std::optional<DatasetVersionId> dataset)
{

    return query(connection,
        "UPDATE training_candidates SET state=$3,dataset_version_id=$4::bigint "
        "WHERE id=$1::bigint AND state=$2 RETURNING 1",
        {number(id), trainingState(expected), trainingState(next),
         dataset ? std::optional<std::string>{number(*dataset)} : std::nullopt}
    ).rows() == 1;
}

DatasetVersion PostgresTrainingRepository::createDatasetVersion(PGconn *connection, bool ownsTransaction, DatasetVersion version)
{

    const bool localTransaction = ownsTransaction;
    if (localTransaction) command(connection, "BEGIN");
    try
    {
        const auto result = query(connection,
            "INSERT INTO dataset_versions(name,checksum,created_at,artifact_path,record_count) "
            "VALUES($1,$2,to_timestamp($3::double precision/1000000.0),$4,$5::bigint) RETURNING " +
            std::string(kDatasetVersionColumns),
            {version.name, version.checksum, encodeTime(version.createdAt),
             version.artifactPath, number(version.recordCount)});
        auto persisted = readDatasetVersion(result, 0);
        persisted.candidates = version.candidates;
        for (const auto candidate : version.candidates)
            (void)query(connection,
                "INSERT INTO dataset_candidates(dataset_version_id,candidate_id) VALUES($1::bigint,$2::bigint) RETURNING 1",
                {number(persisted.id), number(candidate)});
        if (localTransaction) command(connection, "COMMIT");
        return persisted;
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

std::optional<DatasetVersion> PostgresTrainingRepository::findDatasetVersion(PGconn *connection, DatasetVersionId id)
{

    const auto result = query(connection,
        "SELECT " + std::string(kDatasetVersionColumns) +
        " FROM dataset_versions WHERE id=$1::bigint", {number(id)});
    if (!result.rows()) return std::nullopt;
    auto version = readDatasetVersion(result, 0);
    const auto candidates = query(connection,
        "SELECT candidate_id FROM dataset_candidates "
        "WHERE dataset_version_id=$1::bigint ORDER BY candidate_id",
        {number(id)});
    version.candidates.reserve(candidates.rows());
    for (int row = 0; row < candidates.rows(); ++row)
        version.candidates.push_back(std::stoull(candidates.value(row, 0)));
    return version;
}

}
