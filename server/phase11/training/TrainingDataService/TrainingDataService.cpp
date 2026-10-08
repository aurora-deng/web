#include "server/phase11/training/TrainingDataService/TrainingDataService.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>

namespace webserver::phase11
{

TrainingCandidate TrainingDataService::submitTrainingCandidate(
    TrainingSubmission submission, TimePoint now)
{
    if (!submission.consentGranted || submission.consentVersion.empty())
        throw ApplicationError(ErrorCode::Forbidden,
                               "explicit training consent is required");
    const auto source = messages_.findMessage(submission.sourceMessageId);
    if (!source)
        throw ApplicationError(ErrorCode::NotFound,
                               "source message not found");
    const auto conversation = conversations_.findConversation(
        source->conversationId);
    if (!conversation || conversation->kind != ConversationKind::Direct ||
        conversation->state != ConversationState::Active ||
        !conversations_.findMember(source->conversationId,
                                   submission.ownerUserId))
        throw ApplicationError(ErrorCode::Forbidden,
                               "training source must be an active direct conversation");
    const auto members = conversations_.listMembers(source->conversationId);
    const bool hasEnabledAiPeer = members.size() == 2 &&
        std::any_of(members.begin(), members.end(), [&](const auto &member)
        {
            if (member.userId == submission.ownerUserId) return false;
            const auto peer = users_.findUser(member.userId);
            return peer && peer->aiAccount && !peer->disabled;
        });
    if (!hasEnabledAiPeer)
        throw ApplicationError(ErrorCode::Forbidden,
                               "training source must be a direct AI conversation");
    if (training_.findActiveTrainingCandidate(
            submission.ownerUserId, submission.sourceMessageId))
        throw ApplicationError(ErrorCode::Conflict,
                               "this AI response was already submitted for training");
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

std::vector<TrainingCandidate> TrainingDataService::trainingCandidates(
    UserId owner) const
{
    return training_.trainingCandidatesForOwner(owner);
}

TrainingCandidate TrainingDataService::reviewTrainingCandidate(
    TrainingCandidateId candidate, bool approve)
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
    if (!training_.transitionTrainingCandidate(
            found->id, TrainingCandidateState::Submitted, found->state))
        throw ApplicationError(ErrorCode::Conflict,
                               "training candidate changed during review");
    return *found;
}

TrainingCandidate TrainingDataService::revokeTrainingCandidate(
    TrainingCandidateId candidate, UserId owner)
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
    const auto previous = found->state;
    found->state = TrainingCandidateState::Revoked;
    if (!training_.transitionTrainingCandidate(
            found->id, previous, TrainingCandidateState::Revoked,
            found->datasetVersionId))
        throw ApplicationError(ErrorCode::Conflict,
                               "training candidate changed during revocation");
    return *found;
}

DatasetVersion TrainingDataService::createDataset(
    std::string name, std::vector<TrainingCandidateId> candidateIds,
    TimePoint now, ITrainingDataExporter &exporter)
{
    if (name.empty() || name.size() > 120 || candidateIds.empty())
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "dataset name and candidates are required");
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

    // 先生成内容寻址的不可变文件，再开始数据库事务。导出失败不会留下 Dataset
    // 记录；数据库失败最多留下一个未引用文件，可按 checksum 安全复用或清理。
    const auto artifact = exporter.exportDataset(name, candidates);
    const bool checksumValid = artifact.sha256.size() == 64 &&
        std::all_of(artifact.sha256.begin(), artifact.sha256.end(),
                    [](unsigned char value) { return std::isxdigit(value); });
    const std::filesystem::path artifactPath(artifact.relativePath);
    if (!checksumValid || artifact.recordCount != candidates.size() ||
        artifactPath.empty() || artifactPath.is_absolute() ||
        artifactPath.has_root_path() ||
        std::any_of(artifactPath.begin(), artifactPath.end(),
                    [](const auto &part) { return part == ".."; }))
        throw ApplicationError(ErrorCode::Unavailable,
                               "dataset exporter returned an invalid artifact");

    auto transaction = database_.beginTransaction();
    // 在事务内重读状态，避免导出期间撤销同意却仍被标记为 Exported。
    for (const auto id : candidateIds)
    {
        const auto current = training_.findTrainingCandidate(id);
        if (!current || current->state != TrainingCandidateState::Approved)
            throw ApplicationError(ErrorCode::Conflict,
                                   "training consent changed during dataset export");
    }
    auto dataset = training_.createDatasetVersion(
        {0, std::move(name), artifact.sha256, candidateIds, now,
         artifact.relativePath, artifact.recordCount});
    for (auto &candidate : candidates)
    {
        if (!training_.transitionTrainingCandidate(
                candidate.id, TrainingCandidateState::Approved,
                TrainingCandidateState::Exported, dataset.id))
            throw ApplicationError(ErrorCode::Conflict,
                                   "training candidate changed during export");
    }
    transaction->commit();
    return dataset;
}

} // namespace webserver::phase11
