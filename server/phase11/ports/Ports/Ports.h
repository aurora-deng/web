#pragma once

#include "server/phase11/domain/Domain/Domain.h"

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace webserver::phase11
{

class ITransaction
{
public:
    virtual ~ITransaction() = default;
    virtual void commit() = 0;
    virtual void rollback() = 0;
};

class IDatabase
{
public:
    virtual ~IDatabase() = default;
    [[nodiscard]] virtual std::unique_ptr<ITransaction> beginTransaction() = 0;
    [[nodiscard]] virtual bool healthy() const noexcept = 0;
};

class IUserRepository
{
public:
    virtual ~IUserRepository() = default;
    virtual User create(User user) = 0;
    [[nodiscard]] virtual std::optional<User> findUser(UserId id) const = 0;
    [[nodiscard]] virtual std::optional<User> findUserByName(
        std::string_view username) const = 0;
    virtual bool updateProfile(UserId id, std::string displayName,
                               std::string biography) = 0;
};

class ISessionRepository
{
public:
    virtual ~ISessionRepository() = default;
    virtual Session createSession(Session session) = 0;
    [[nodiscard]] virtual std::optional<Session> findSessionByTokenHash(
        std::string_view tokenHash) const = 0;
    virtual bool revokeSession(std::string_view sessionId, TimePoint now) = 0;
};

class ISocialRepository
{
public:
    virtual ~ISocialRepository() = default;
    virtual FriendRequest createFriendRequest(FriendRequest request) = 0;
    [[nodiscard]] virtual std::optional<FriendRequest> findFriendRequest(
        FriendRequestId id) const = 0;
    virtual bool decideFriendRequest(FriendRequestId id, UserId receiver,
                                     FriendRequestState decision, TimePoint now) = 0;
    [[nodiscard]] virtual bool areFriends(UserId first, UserId second) const = 0;
    [[nodiscard]] virtual std::vector<UserId> listFriendIds(UserId user) const = 0;
    [[nodiscard]] virtual std::vector<FriendRequest> pendingRequests(
        UserId receiver) const = 0;
};

class IConversationRepository
{
public:
    virtual ~IConversationRepository() = default;
    virtual Conversation createConversation(
        Conversation conversation,
        std::vector<ConversationMember> members) = 0;
    [[nodiscard]] virtual std::optional<Conversation> findConversation(
        ConversationId id) const = 0;
    [[nodiscard]] virtual std::optional<ConversationMember> findMember(
        ConversationId conversation, UserId user) const = 0;
    [[nodiscard]] virtual std::optional<Conversation> findDirectConversation(
        UserId first, UserId second) const = 0;
    [[nodiscard]] virtual std::vector<Conversation> listConversations(
        UserId user) const = 0;
    [[nodiscard]] virtual std::vector<ConversationMember> listMembers(
        ConversationId conversation) const = 0;
    virtual bool addMember(ConversationMember member) = 0;
    virtual bool updateDelivered(ConversationId conversation, UserId user,
                                 std::uint64_t sequence) = 0;
    virtual bool updateRead(ConversationId conversation, UserId user,
                            std::uint64_t sequence) = 0;
};

class IMessageRepository
{
public:
    virtual ~IMessageRepository() = default;
    virtual AppendMessageResult appendMessage(Message message) = 0;
    [[nodiscard]] virtual std::vector<Message> messagesAfter(
        ConversationId conversation, std::uint64_t sequence,
        std::size_t limit) const = 0;
    [[nodiscard]] virtual std::vector<Message> searchMessages(
        ConversationId conversation, std::string_view query,
        std::size_t limit) const = 0;
    [[nodiscard]] virtual std::optional<Message> findMessage(MessageId id) const = 0;
};

class IOutboxRepository
{
public:
    virtual ~IOutboxRepository() = default;
    virtual OutboxEvent appendOutbox(OutboxEvent event) = 0;
    [[nodiscard]] virtual std::vector<OutboxEvent> pendingOutbox(
        std::size_t limit) const = 0;
    virtual bool markPublished(OutboxEventId id, TimePoint now) = 0;
};

class IModelRepository
{
public:
    virtual ~IModelRepository() = default;
    virtual ModelNode createModelNode(ModelNode node) = 0;
    virtual ModelVersion createModelVersion(ModelVersion version) = 0;
    virtual ModelEdge createModelEdge(ModelEdge edge) = 0;
    virtual bool bindAiAccount(ModelBinding binding) = 0;
    [[nodiscard]] virtual std::optional<ModelNode> findModelNode(ModelNodeId id) const = 0;
    [[nodiscard]] virtual std::optional<ModelVersion> findModelVersion(
        ModelVersionId id) const = 0;
    [[nodiscard]] virtual std::optional<ModelBinding> findModelBinding(
        UserId aiUser) const = 0;
    [[nodiscard]] virtual std::vector<ModelEdge> modelEdges() const = 0;
    virtual bool activateModelVersion(ModelNodeId node, ModelVersionId version) = 0;
};

class ITrainingRepository
{
public:
    virtual ~ITrainingRepository() = default;
    virtual TrainingCandidate createTrainingCandidate(TrainingCandidate candidate) = 0;
    [[nodiscard]] virtual std::optional<TrainingCandidate> findTrainingCandidate(
        TrainingCandidateId id) const = 0;
    virtual bool updateTrainingCandidate(TrainingCandidate candidate) = 0;
    virtual DatasetVersion createDatasetVersion(DatasetVersion version) = 0;
};

/**
 * 密码与随机令牌依赖被抽象成端口。正式实现以后由 libsodium 提供；单元测试使用
 * 明确标注为 TestOnly 的实现，避免把不安全散列误放进生产路径。
 */
class ICredentialCodec
{
public:
    virtual ~ICredentialCodec() = default;
    [[nodiscard]] virtual std::string hashPassword(std::string_view password) const = 0;
    [[nodiscard]] virtual std::string dummyPasswordHash() const = 0;
    [[nodiscard]] virtual bool verifyPassword(std::string_view encoded,
                                              std::string_view password) const = 0;
    [[nodiscard]] virtual std::string randomToken() const = 0;
    [[nodiscard]] virtual std::string hashToken(std::string_view token) const = 0;
    [[nodiscard]] virtual bool verifyToken(std::string_view encoded,
                                           std::string_view token) const = 0;
};

/** 登录限流端口。生产环境以后可换成 PostgreSQL/Redis 实现，不改变 AuthService。 */
class ILoginRateLimiter
{
public:
    virtual ~ILoginRateLimiter() = default;
    [[nodiscard]] virtual bool allowLogin(std::string_view username,
                                          std::string_view origin,
                                          TimePoint now) = 0;
    virtual void recordFailure(std::string_view username,
                               std::string_view origin,
                               TimePoint now) = 0;
    virtual void recordSuccess(std::string_view username,
                               std::string_view origin,
                               TimePoint now) = 0;
};

} // namespace webserver::phase11
