#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include "backend/application/domain/FileTypes/FileTypes.h"

namespace webserver::phase11 {
/** MessageTypes: protocol/database independent business facts. */
struct MessageModelTrace
{
    std::optional<ModelNodeId> nodeId;
    std::optional<ModelVersionId> versionId;
    std::string adapterName;
};

enum class MessageState
{
    Active,
    Recalled
};

struct Message
{
    MessageId id = 0;
    ConversationId conversationId = 0;
    std::uint64_t sequence = 0;
    UserId senderId = 0;
    std::string clientMessageId;
    std::string body;
    MessageModelTrace modelTrace;
    TimePoint createdAt{};
    MessageState state = MessageState::Active;
    std::optional<UserId> recalledBy;
    std::optional<TimePoint> recalledAt;
    std::vector<MessageAttachment> attachments;
};

struct AppendMessageResult
{
    Message message;
    bool inserted = false;
};

struct RecallMessageResult
{
    Message message;
    bool changed = false;
};
}
