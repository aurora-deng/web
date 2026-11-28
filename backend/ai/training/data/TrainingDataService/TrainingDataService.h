#pragma once

#include "backend/application/modules/shared/domain/ApplicationError/ApplicationError.h"
#include "backend/application/modules/ai/domain/TrainingSubmission/TrainingSubmission.h"
#include "backend/application/modules/ai/ports/ITrainingBusiness/ITrainingBusiness.h"
#include "backend/application/modules/shared/ports/IDatabase/IDatabase.h"
#include "backend/application/modules/accounts/ports/IUserRepository/IUserRepository.h"
#include "backend/application/modules/chat/ports/IConversationRepository/IConversationRepository.h"
#include "backend/application/modules/chat/ports/IMessageRepository/IMessageRepository.h"
#include "backend/application/modules/ai/ports/ITrainingRepository/ITrainingRepository.h"
#include "backend/application/modules/ai/domain/TrainingTypes/TrainingTypes.h"

namespace webserver::phase11
{

struct TrainingDatasetArtifact
{
    std::string relativePath;
    std::string sha256;
    std::uint64_t recordCount = 0;
};

class ITrainingDataExporter
{
public:
    virtual ~ITrainingDataExporter() = default;
    virtual TrainingDatasetArtifact exportDataset(
        std::string_view datasetName,
        const std::vector<TrainingCandidate> &candidates) = 0;
};

class TrainingDataService final : public ITrainingBusiness
{
public:
    TrainingDataService(IDatabase &database,
                        IUserRepository &users,
                        IMessageRepository &messages,
                        IConversationRepository &conversations,
                        ITrainingRepository &training)
        : database_(database), users_(users), messages_(messages),
          conversations_(conversations), training_(training)
    {
    }

    TrainingCandidate submitTrainingCandidate(
        TrainingSubmission submission, TimePoint now) override;
    [[nodiscard]] std::vector<TrainingCandidate> trainingCandidates(
        UserId owner) const override;
    TrainingCandidate revokeTrainingCandidate(
        TrainingCandidateId candidate, UserId owner) override;

    TrainingCandidate reviewTrainingCandidate(
        TrainingCandidateId candidate, bool approve);
    DatasetVersion createDataset(std::string name,
                                 std::vector<TrainingCandidateId> candidates,
                                 TimePoint now,
                                 ITrainingDataExporter &exporter);

private:
    IDatabase &database_;
    IUserRepository &users_;
    IMessageRepository &messages_;
    IConversationRepository &conversations_;
    ITrainingRepository &training_;
};

} // namespace webserver::phase11
