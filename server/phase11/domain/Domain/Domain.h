#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace webserver::phase11
{

using UserId = std::uint64_t;
using FriendRequestId = std::uint64_t;
using ConversationId = std::uint64_t;
using MessageId = std::uint64_t;
using OutboxEventId = std::uint64_t;
using ModelNodeId = std::uint64_t;
using ModelVersionId = std::uint64_t;
using ModelEdgeId = std::uint64_t;
using TrainingCandidateId = std::uint64_t;
using DatasetVersionId = std::uint64_t;
using TimePoint = std::chrono::system_clock::time_point;

/**
 * Phase 11 的领域对象只描述“业务事实”，不包含 PostgreSQL、HTTP 或 WebSocket 类型。
 * 可以把它们理解成一套统一货物规格：仓库以后从 PostgreSQL 换成 MySQL，货物本身
 * 仍保持相同形状，因此上层业务无需跟着重写。
 */
struct User
{
    UserId id = 0;
    std::string username;
    std::string displayName;
    std::string biography;
    std::string passwordHash;
    bool disabled = false;
    bool aiAccount = false;
    TimePoint createdAt{};
};

struct Session
{
    std::string id;
    UserId userId = 0;
    std::string tokenHash;
    std::string csrfHash;
    TimePoint createdAt{};
    TimePoint expiresAt{};
    std::optional<TimePoint> revokedAt;
};

enum class FriendRequestState
{
    Pending,
    Accepted,
    Rejected
};

struct FriendRequest
{
    FriendRequestId id = 0;
    UserId senderId = 0;
    UserId receiverId = 0;
    FriendRequestState state = FriendRequestState::Pending;
    TimePoint createdAt{};
    std::optional<TimePoint> decidedAt;
};

/**
 * 联系人设置是单向事实：Alice 给 Bob 写的备注、是否免打扰，只影响 Alice 自己。
 * 好友关系仍由 friendships 维护为双向关系，避免把两个概念混在一张表里。
 */
struct ContactSettings
{
    UserId ownerId = 0;
    UserId contactId = 0;
    std::string remark;
    bool notificationsMuted = false;
};

/** HTTP 联系人列表需要的持久视图；online/连接数由实时适配层现场补充。 */
struct FriendContact
{
    User user;
    ContactSettings settings;
    std::optional<TimePoint> lastActiveAt;
};

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

/**
 * 会话列表所需的持久视图。未读数不是浏览器临时猜测，而是由会话最新序号和
 * 当前用户的已读游标计算出来，因此刷新页面或更换设备后仍然一致。
 */
struct ConversationSummary
{
    Conversation conversation;
    std::uint64_t lastDeliveredSequence = 0;
    std::uint64_t lastReadSequence = 0;
    std::uint64_t unreadCount = 0;
};

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
};

/** 消息幂等写入结果同时属于 Repository 与业务层的公共领域数据。 */
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

struct OutboxEvent
{
    OutboxEventId id = 0;
    std::string type;
    std::string aggregateType;
    std::uint64_t aggregateId = 0;
    std::string payload;
    TimePoint createdAt{};
    std::optional<TimePoint> publishedAt;
};

enum class ModelNodeKind
{
    Root,
    Specialist
};

struct ModelNode
{
    ModelNodeId id = 0;
    std::string name;
    ModelNodeKind kind = ModelNodeKind::Specialist;
    std::optional<ModelVersionId> activeVersionId;
    TimePoint createdAt{};
};

enum class ModelVersionState
{
    Candidate,
    Active,
    Retired,
    Rejected
};

struct ModelVersion
{
    ModelVersionId id = 0;
    ModelNodeId nodeId = 0;
    std::string runtime;       // ollama / onnx-inprocess / onnx-grpc
    std::string modelArtifact;
    std::string adapterArtifact;
    std::string checksum;
    ModelVersionState state = ModelVersionState::Candidate;
    TimePoint createdAt{};
};

enum class ModelEdgeKind
{
    ParentControlsChild,
    PeerCollaborates
};

struct ModelEdge
{
    ModelEdgeId id = 0;
    ModelNodeId from = 0;
    ModelNodeId to = 0;
    ModelEdgeKind kind = ModelEdgeKind::PeerCollaborates;
};

struct ModelBinding
{
    UserId aiUserId = 0;
    ModelNodeId nodeId = 0;
};

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
};

} // namespace webserver::phase11
