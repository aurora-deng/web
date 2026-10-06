#include "server/phase11/training/TrainingDataService/TrainingDataService.h"

#include <set>

namespace webserver::phase11
{

TrainingCandidate TrainingDataService::submit(
    TrainingSubmission submission, TimePoint now)
{
    if (!submission.consentGranted || submission.consentVersion.empty())
        throw ApplicationError(ErrorCode::Forbidden,
                               "explicit training consent is required");
    const auto source = messages_.findMessage(submission.sourceMessageId);
    if (!source)
        throw ApplicationError(ErrorCode::NotFound,
                               "source message not found");
    if (!conversations_.findMember(source->conversationId,
                                   submission.ownerUserId))
        throw ApplicationError(ErrorCode::Forbidden,
                               "training owner cannot access the source message");
    if (submission.sanitizedPrompt.empty() ||
        submission.sanitizedResponse.empty() ||
        submission.sanitizedPrompt.size() > 32 * 1024 ||
        submission.sanitizedResponse.size() > 32 * 1024)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "sanitized training pair is invalid");

    return training_.createTrainingCandidate(
        {0, submission.ownerUserId, submission.sourceMessageId,
         std::move(submission.sanitizedPrompt),
         std::move(submission.sanitizedResponse),
         std::move(submission.consentVersion),
         TrainingCandidateState::Submitted, {}, now});
}

TrainingCandidate TrainingDataService::review(TrainingCandidateId candidate,
                                              bool approve)
{
    auto found = training_.findTrainingCandidate(candidate);
    if (!found)
        throw ApplicationError(ErrorCode::NotFound,
                               "training candidate not found");
    if (found->state != TrainingCandidateState::Submitted)
        throw ApplicationError(ErrorCode::Conflict,
                               "training candidate was already reviewed");
    found->state = approve ? TrainingCandidateState::Approved
                           : TrainingCandidateState::Rejected;
    training_.updateTrainingCandidate(*found);
    return *found;
}

TrainingCandidate TrainingDataService::revoke(TrainingCandidateId candidate,
                                              UserId owner)
{
    auto found = training_.findTrainingCandidate(candidate);
    if (!found)
        throw ApplicationError(ErrorCode::NotFound,
                               "training candidate not found");
    if (found->ownerUserId != owner)
        throw ApplicationError(ErrorCode::Forbidden,
                               "only the owner can revoke consent");
    // 已导出的不可变数据集不能被悄悄改写，但撤销记录仍必须落库。后续训练只选择
    // 未撤销样本，并通过新数据集版本表达删除，保留完整数据血缘。
    found->state = TrainingCandidateState::Revoked;
    training_.updateTrainingCandidate(*found);
    return *found;
}

DatasetVersion TrainingDataService::createDataset(
    std::string name, std::vector<TrainingCandidateId> candidateIds,
    std::string checksum, TimePoint now,
    ITrainingDataExporter &exporter)
{
    if (name.empty() || checksum.empty() || candidateIds.empty())
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "dataset name, checksum and candidates are required");
    const std::set<TrainingCandidateId> unique(
        candidateIds.begin(), candidateIds.end());
    if (unique.size() != candidateIds.size())
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "dataset contains duplicate candidates");

    std::vector<TrainingCandidate> candidates;
    candidates.reserve(candidateIds.size());
    for (const auto id : candidateIds)
    {
        const auto candidate = training_.findTrainingCandidate(id);
        if (!candidate || candidate->state != TrainingCandidateState::Approved)
            throw ApplicationError(ErrorCode::Conflict,
                                   "dataset requires approved candidates");
        candidates.push_back(*candidate);
    }

    auto transaction = database_.beginTransaction();
    auto dataset = training_.createDatasetVersion(
        {0, std::move(name), std::move(checksum), candidateIds, now});
    transaction->commit();

    // 文件导出属于数据库外部副作用，放在提交之后。导出失败时候选仍保持 Approved，
    // 可以安全重试；绝不能把数据库回滚假装成已经撤销了磁盘写入。
    const auto artifact = exporter.exportDataset(dataset, candidates);
    if (artifact.empty())
        throw ApplicationError(ErrorCode::Unavailable,
                               "dataset exporter returned no artifact");

    auto markTransaction = database_.beginTransaction();
    for (auto &candidate : candidates)
    {
        candidate.state = TrainingCandidateState::Exported;
        candidate.datasetVersionId = dataset.id;
        training_.updateTrainingCandidate(candidate);
    }
    markTransaction->commit();
    return dataset;
}

} // namespace webserver::phase11
