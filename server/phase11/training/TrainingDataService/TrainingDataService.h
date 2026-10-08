#pragma once

#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/business/BusinessInterfaces/BusinessInterfaces.h"
#include "server/phase11/ports/Ports/Ports.h"

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
