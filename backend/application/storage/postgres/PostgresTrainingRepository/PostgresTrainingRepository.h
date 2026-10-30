#pragma once
#include "backend/application/domain/TrainingTypes/TrainingTypes.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
#include "backend/application/storage/postgres/PostgresOutboxWriter/PostgresOutboxWriter.h"
namespace webserver::phase11 {
/** Training SQL adapter. Uses the caller's connection; never borrows another lease. */
class PostgresTrainingRepository final {
public:
    static TrainingCandidate createTrainingCandidate(PGconn *connection, TrainingCandidate candidate);
    static std::optional<TrainingCandidate> findTrainingCandidate(PGconn *connection, TrainingCandidateId id);
    static std::optional<TrainingCandidate> findActiveTrainingCandidate(PGconn *connection, UserId owner, MessageId sourceMessage);
    static std::vector<TrainingCandidate> trainingCandidatesForOwner(PGconn *connection, UserId owner);
    static std::vector<TrainingCandidate> trainingCandidatesByState(PGconn *connection, TrainingCandidateState state);
    static bool updateTrainingCandidate(PGconn *connection, TrainingCandidate candidate);
    static bool transitionTrainingCandidate(PGconn *connection, TrainingCandidateId id, TrainingCandidateState expected, TrainingCandidateState next, std::optional<DatasetVersionId> dataset);
    static DatasetVersion createDatasetVersion(PGconn *connection, bool ownsTransaction, DatasetVersion version);
    static std::optional<DatasetVersion> findDatasetVersion(PGconn *connection, DatasetVersionId id);
};
}
