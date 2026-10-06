#pragma once

#include "server/phase11/ports/Ports/Ports.h"

#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>

namespace webserver::phase11
{

/**
 * 无外部依赖的参考仓库。
 *
 * 它有两个用途：
 *  1. PostgreSQL 尚未安装时，先验证业务规则和 Repository 契约；
 *  2. PostgreSQL 实现完成后，让两种实现跑同一套测试，防止数据库细节改变业务语义。
 *
 * 事务通过“进入事务时复制一份状态，回滚时恢复”实现。这适合测试与学习，不适合生产
 * 大数据量；生产版会把相同 ITransaction 契约映射到 PostgreSQL BEGIN/COMMIT/ROLLBACK。
 */
class InMemoryStore final : public IDatabase,
                            public IUserRepository,
                            public ISessionRepository,
                            public ISocialRepository,
                            public IConversationRepository,
                            public IMessageRepository,
                            public IOutboxRepository,
                            public IModelRepository,
                            public ITrainingRepository
{
public:
    InMemoryStore() = default;
    ~InMemoryStore() override = default;

    InMemoryStore(const InMemoryStore &) = delete;
    InMemoryStore &operator=(const InMemoryStore &) = delete;

    std::unique_ptr<ITransaction> beginTransaction() override;
    bool healthy() const noexcept override { return true; }

    User create(User user) override;
    std::optional<User> findUser(UserId id) const override;
    std::optional<User> findUserByName(std::string_view username) const override;
    bool updateProfile(UserId id, std::string displayName,
                       std::string biography) override;

    Session createSession(Session session) override;
    std::optional<Session> findSessionByTokenHash(
        std::string_view tokenHash) const override;
    bool revokeSession(std::string_view sessionId, TimePoint now) override;

    FriendRequest createFriendRequest(FriendRequest request) override;
    std::optional<FriendRequest> findFriendRequest(FriendRequestId id) const override;
    bool decideFriendRequest(FriendRequestId id, UserId receiver,
                             FriendRequestState decision, TimePoint now) override;
    bool areFriends(UserId first, UserId second) const override;
    std::vector<UserId> listFriendIds(UserId user) const override;
    std::vector<FriendRequest> pendingRequests(UserId receiver) const override;

    Conversation createConversation(
        Conversation conversation,
        std::vector<ConversationMember> members) override;
    std::optional<Conversation> findConversation(ConversationId id) const override;
    std::optional<ConversationMember> findMember(
        ConversationId conversation, UserId user) const override;
    std::optional<Conversation> findDirectConversation(
        UserId first, UserId second) const override;
    std::vector<Conversation> listConversations(UserId user) const override;
    std::vector<ConversationMember> listMembers(
        ConversationId conversation) const override;
    bool addMember(ConversationMember member) override;
    bool updateDelivered(ConversationId conversation, UserId user,
                         std::uint64_t sequence) override;
    bool updateRead(ConversationId conversation, UserId user,
                    std::uint64_t sequence) override;

    AppendMessageResult appendMessage(Message message) override;
    std::vector<Message> messagesAfter(ConversationId conversation,
                                       std::uint64_t sequence,
                                       std::size_t limit) const override;
    std::vector<Message> searchMessages(ConversationId conversation,
                                        std::string_view query,
                                        std::size_t limit) const override;
    std::optional<Message> findMessage(MessageId id) const override;

    OutboxEvent appendOutbox(OutboxEvent event) override;
    std::vector<OutboxEvent> pendingOutbox(std::size_t limit) const override;
    bool markPublished(OutboxEventId id, TimePoint now) override;

    ModelNode createModelNode(ModelNode node) override;
    ModelVersion createModelVersion(ModelVersion version) override;
    ModelEdge createModelEdge(ModelEdge edge) override;
    bool bindAiAccount(ModelBinding binding) override;
    std::optional<ModelNode> findModelNode(ModelNodeId id) const override;
    std::optional<ModelVersion> findModelVersion(ModelVersionId id) const override;
    std::optional<ModelBinding> findModelBinding(UserId aiUser) const override;
    std::vector<ModelEdge> modelEdges() const override;
    bool activateModelVersion(ModelNodeId node, ModelVersionId version) override;

    TrainingCandidate createTrainingCandidate(TrainingCandidate candidate) override;
    std::optional<TrainingCandidate> findTrainingCandidate(
        TrainingCandidateId id) const override;
    bool updateTrainingCandidate(TrainingCandidate candidate) override;
    DatasetVersion createDatasetVersion(DatasetVersion version) override;

private:
    using UserPair = std::pair<UserId, UserId>;
    using MemberKey = std::pair<ConversationId, UserId>;
    using IdempotencyKey = std::tuple<ConversationId, UserId, std::string>;

    struct State
    {
        UserId nextUserId = 1;
        FriendRequestId nextFriendRequestId = 1;
        ConversationId nextConversationId = 1;
        MessageId nextMessageId = 1;
        OutboxEventId nextOutboxEventId = 1;
        ModelNodeId nextModelNodeId = 1;
        ModelVersionId nextModelVersionId = 1;
        ModelEdgeId nextModelEdgeId = 1;
        TrainingCandidateId nextTrainingCandidateId = 1;
        DatasetVersionId nextDatasetVersionId = 1;

        std::unordered_map<UserId, User> users;
        std::unordered_map<std::string, UserId> usernames;
        std::unordered_map<std::string, Session> sessionsById;
        std::unordered_map<std::string, std::string> sessionIdByTokenHash;
        std::unordered_map<FriendRequestId, FriendRequest> friendRequests;
        std::set<UserPair> friendships;
        std::unordered_map<ConversationId, Conversation> conversations;
        std::map<MemberKey, ConversationMember> members;
        std::unordered_map<MessageId, Message> messages;
        std::unordered_map<ConversationId, std::vector<MessageId>> messageIds;
        std::map<IdempotencyKey, MessageId> idempotency;
        std::unordered_map<OutboxEventId, OutboxEvent> outbox;
        std::unordered_map<ModelNodeId, ModelNode> modelNodes;
        std::unordered_map<ModelVersionId, ModelVersion> modelVersions;
        std::unordered_map<ModelEdgeId, ModelEdge> modelEdges;
        std::unordered_map<UserId, ModelBinding> modelBindings;
        std::unordered_map<TrainingCandidateId, TrainingCandidate> trainingCandidates;
        std::unordered_map<DatasetVersionId, DatasetVersion> datasets;
    };

    class Transaction;

    static UserPair orderedUsers(UserId first, UserId second);

    mutable std::recursive_mutex mutex_;
    State state_;
};

} // namespace webserver::phase11
