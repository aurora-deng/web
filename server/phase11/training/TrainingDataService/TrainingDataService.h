#pragma once

#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/ports/Ports/Ports.h"

namespace webserver::phase11
{

class ITrainingDataExporter
{
public:
    virtual ~ITrainingDataExporter() = default;
    virtual std::string exportDataset(
        const DatasetVersion &dataset,
        const std::vector<TrainingCandidate> &candidates) = 0;
};

struct TrainingSubmission
{
    UserId ownerUserId = 0;
    MessageId sourceMessageId = 0;
    std::string sanitizedPrompt;
    std::string sanitizedResponse;
    std::string consentVersion;
    bool consentGranted = false;
};

class TrainingDataService
{
public:
    TrainingDataService(IDatabase &database,
                        IMessageRepository &messages,
                        IConversationRepository &conversations,
                        ITrainingRepository &training)
        : database_(database), messages_(messages),
          conversations_(conversations), training_(training)
    {
    }

    TrainingCandidate submit(TrainingSubmission submission, TimePoint now);
    TrainingCandidate review(TrainingCandidateId candidate, bool approve);
    TrainingCandidate revoke(TrainingCandidateId candidate, UserId owner);
    DatasetVersion createDataset(std::string name,
                                 std::vector<TrainingCandidateId> candidates,
                                 std::string checksum,
                                 TimePoint now,
                                 ITrainingDataExporter &exporter);

private:
    IDatabase &database_;
    IMessageRepository &messages_;
    IConversationRepository &conversations_;
    ITrainingRepository &training_;
};

} // namespace webserver::phase11
