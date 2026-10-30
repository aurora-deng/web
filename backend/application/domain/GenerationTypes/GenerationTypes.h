#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"

namespace webserver::phase11 {
/** GenerationTypes: protocol/database independent business facts. */
enum class AiGenerationStatus
{
    Queued,
    Running,
    Succeeded,
    Failed,
    Cancelled,
    TimedOut
};

struct AiGenerationAudit
{
    std::string generationId;
    UserId requesterId = 0;
    UserId aiUserId = 0;
    ConversationId conversationId = 0;
    std::optional<ModelNodeId> modelNodeId;
    std::optional<ModelVersionId> modelVersionId;
    std::string runtime;
    std::string modelArtifact;
    std::string adapterArtifact;
    std::string modelChecksum;
    std::string promptSha256;
    std::uint64_t promptBytes = 0;
    std::string outputSha256;
    std::uint64_t outputBytes = 0;
    std::uint64_t outputTokens = 0;
    AiGenerationStatus status = AiGenerationStatus::Queued;
    std::string errorDetail;
    TimePoint requestedAt{};
    std::optional<TimePoint> startedAt;
    std::optional<TimePoint> firstTokenAt;
    std::optional<TimePoint> completedAt;
    std::optional<MessageId> responseMessageId;
    // 异步数据库任务可能乱序完成；revision 保证旧快照不能覆盖新状态。
    std::uint64_t revision = 1;
};
}
