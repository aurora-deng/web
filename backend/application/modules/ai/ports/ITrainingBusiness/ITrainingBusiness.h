#pragma once
#include "backend/application/modules/ai/domain/TrainingTypes/TrainingTypes.h"
#include "backend/application/modules/ai/domain/TrainingSubmission/TrainingSubmission.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webserver::phase11 {

class ITrainingBusiness
{
public:
    virtual ~ITrainingBusiness() = default;

    virtual TrainingCandidate submitTrainingCandidate(
        TrainingSubmission submission, TimePoint now) = 0;
    [[nodiscard]] virtual std::vector<TrainingCandidate>
    trainingCandidates(UserId owner) const = 0;
    virtual TrainingCandidate revokeTrainingCandidate(
        TrainingCandidateId candidate, UserId owner) = 0;
};

} // namespace webserver::phase11
