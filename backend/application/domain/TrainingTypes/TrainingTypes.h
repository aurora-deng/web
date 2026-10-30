#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"

namespace webserver::phase11 {
/** TrainingTypes: protocol/database independent business facts. */
enum class TrainingCandidateState
{
    Submitted,
    Approved,
    Rejected,
    Exported,
    Revoked
};

struct TrainingCandidate
{
    TrainingCandidateId id = 0;
    UserId ownerUserId = 0;
    MessageId sourceMessageId = 0;
    std::string sanitizedPrompt;
    std::string sanitizedResponse;
    std::string consentVersion;
    TrainingCandidateState state = TrainingCandidateState::Submitted;
    std::optional<DatasetVersionId> datasetVersionId;
    TimePoint createdAt{};
};

struct DatasetVersion
{
    DatasetVersionId id = 0;
    std::string name;
    std::string checksum;
    std::vector<TrainingCandidateId> candidates;
    TimePoint createdAt{};
    // 数据集正文保存在受控训练目录，数据库只登记相对路径、记录数和指纹。
    std::string artifactPath;
    std::uint64_t recordCount = 0;
};
}
