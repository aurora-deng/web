#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"

namespace webserver::phase11 {
/** ConversationTypes: protocol/database independent business facts. */
enum class ConversationKind
{
    Direct,
    Group
};

enum class MemberRole
{
    Member,
    Administrator,
    Owner
};

enum class ConversationState
{
    Active,
    Dissolved
};

struct Conversation
{
    ConversationId id = 0;
    ConversationKind kind = ConversationKind::Direct;
    std::string title;
    UserId createdBy = 0;
    std::uint64_t nextSequence = 1;
    TimePoint createdAt{};
    std::string announcement;
    ConversationState state = ConversationState::Active;
    std::optional<UserId> dissolvedBy;
    std::optional<TimePoint> dissolvedAt;
};

struct ConversationMember
{
    ConversationId conversationId = 0;
    UserId userId = 0;
    MemberRole role = MemberRole::Member;
    std::uint64_t lastDeliveredSequence = 0;
    std::uint64_t lastReadSequence = 0;
    TimePoint joinedAt{};
    std::optional<TimePoint> mutedUntil;
};

struct ConversationSummary
{
    Conversation conversation;
    std::uint64_t lastDeliveredSequence = 0;
    std::uint64_t lastReadSequence = 0;
    std::uint64_t unreadCount = 0;
    // 单次列表查询的投影，不能带密码散列等完整 User 数据。
    std::string displayTitle;
    std::optional<FileId> avatarFileId;
    std::string lastMessagePreview;
    TimePoint lastMessageAt{};
    bool notificationsMuted = false;
    bool containsAi = false;
    std::optional<UserId> peerUserId;
    std::uint64_t preferencesRevision = 0;
};
}
