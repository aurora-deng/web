#pragma once
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include "backend/application/domain/TrainingTypes/TrainingTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace webserver::phase11 {
class ITrainingRepository
{
public:
    virtual ~ITrainingRepository() = default;
    virtual TrainingCandidate createTrainingCandidate(TrainingCandidate candidate) = 0;
    [[nodiscard]] virtual std::optional<TrainingCandidate> findTrainingCandidate(
        TrainingCandidateId id) const = 0;
    [[nodiscard]] virtual std::optional<TrainingCandidate>
    findActiveTrainingCandidate(UserId owner, MessageId sourceMessage) const = 0;
    [[nodiscard]] virtual std::vector<TrainingCandidate>
    trainingCandidatesForOwner(UserId owner) const = 0;
    [[nodiscard]] virtual std::vector<TrainingCandidate>
    trainingCandidatesByState(TrainingCandidateState state) const = 0;
    virtual bool updateTrainingCandidate(TrainingCandidate candidate) = 0;
    virtual bool transitionTrainingCandidate(
        TrainingCandidateId id, TrainingCandidateState expected,
        TrainingCandidateState next,
        std::optional<DatasetVersionId> dataset = std::nullopt) = 0;
    virtual DatasetVersion createDatasetVersion(DatasetVersion version) = 0;
    [[nodiscard]] virtual std::optional<DatasetVersion> findDatasetVersion(
        DatasetVersionId id) const = 0;
};
}
