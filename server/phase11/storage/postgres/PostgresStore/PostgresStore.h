#pragma once

#include "server/phase11/ports/Ports/Ports.h"
#include "server/phase11/storage/postgres/PostgresConnectionPool/PostgresConnectionPool.h"

#include <optional>
#include <thread>

namespace webserver::phase11
{

/**
 * Phase 11 的首个生产存储适配器。公共业务层只看到 Ports.h 中的接口；PGconn、SQL 和
 * libpq 全部收敛在 postgres 目录。一个事务会把连接绑定到发起事务的数据库 Worker，
 * 直到 COMMIT/ROLLBACK 后才归还连接池。
 */
class PostgresStore final : public IDatabase,
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
    explicit PostgresStore(PostgresPoolConfig config);
    ~PostgresStore() override = default;

    PostgresStore(const PostgresStore &) = delete;
    PostgresStore &operator=(const PostgresStore &) = delete;

    std::unique_ptr<ITransaction> beginTransaction() override;
    bool healthy() const noexcept override;

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
    class Transaction;
    struct ConnectionScope
    {
        std::optional<PostgresConnectionPool::Lease> lease;
        PGconn *connection = nullptr;
    };

    [[nodiscard]] ConnectionScope connection() const;

    mutable PostgresConnectionPool pool_;
};

} // namespace webserver::phase11
