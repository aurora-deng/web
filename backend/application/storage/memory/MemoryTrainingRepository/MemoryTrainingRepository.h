#pragma once
#include "backend/application/storage/memory/MemoryState/MemoryState.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
namespace webserver::phase11 {
/** Training in-memory adapter. Facade holds the shared lock during calls. */
class MemoryTrainingRepository final {
public:
    static TrainingCandidate createTrainingCandidate(storage_detail::MemoryState &memory, TrainingCandidate candidate);
    static std::optional<TrainingCandidate> findTrainingCandidate(const storage_detail::MemoryState &memory, TrainingCandidateId id);
    static std::optional<TrainingCandidate> findActiveTrainingCandidate(const storage_detail::MemoryState &memory, UserId owner, MessageId sourceMessage);
    static std::vector<TrainingCandidate> trainingCandidatesForOwner(const storage_detail::MemoryState &memory, UserId owner);
    static std::vector<TrainingCandidate> trainingCandidatesByState(const storage_detail::MemoryState &memory, TrainingCandidateState state);
    static bool updateTrainingCandidate(storage_detail::MemoryState &memory, TrainingCandidate candidate);
    static bool transitionTrainingCandidate(storage_detail::MemoryState &memory, TrainingCandidateId id, TrainingCandidateState expected, TrainingCandidateState next, std::optional<DatasetVersionId> dataset);
    static DatasetVersion createDatasetVersion(storage_detail::MemoryState &memory, DatasetVersion version);
    static std::optional<DatasetVersion> findDatasetVersion(const storage_detail::MemoryState &memory, DatasetVersionId id);
};
}
