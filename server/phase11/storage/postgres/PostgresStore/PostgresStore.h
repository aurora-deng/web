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
                            public IFileRepository,
                            public IOutboxRepository,
                            public IModelRepository,
                            public IAiGenerationAuditRepository,
                            public IModelEvaluationRepository,
                            public IKnowledgeRepository,
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
    bool updateAvatar(UserId id, std::optional<FileId> avatar) override;

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
    bool removeFriendship(UserId first, UserId second) override;
    ContactSettings contactSettings(UserId owner,
                                    UserId contact) const override;
    bool updateContactSettings(UserId owner, UserId contact,
                               std::string remark,
                               bool notificationsMuted) override;
    bool blockUser(UserId blocker, UserId blocked, TimePoint now) override;
    bool unblockUser(UserId blocker, UserId blocked) override;
    bool isBlocked(UserId blocker, UserId blocked) const override;
    bool blockedEitherDirection(UserId first, UserId second) const override;
    std::vector<UserId> listBlockedIds(UserId blocker) const override;
    bool updateLastActive(UserId user, TimePoint now) override;
    std::optional<TimePoint> lastActive(UserId user) const override;

    Conversation createConversation(
        Conversation conversation,
        std::vector<ConversationMember> members) override;
    std::optional<Conversation> findConversation(ConversationId id) const override;
    std::optional<ConversationMember> findMember(
        ConversationId conversation, UserId user) const override;
    std::optional<Conversation> findDirectConversation(
        UserId first, UserId second) const override;
    std::vector<Conversation> listConversations(UserId user) const override;
    std::vector<ConversationSummary> listConversationSummaries(
        UserId user) const override;
    std::vector<ConversationMember> listMembers(
        ConversationId conversation) const override;
    bool addMember(ConversationMember member) override;
    bool updateConversationProfile(ConversationId conversation,
                                   std::string title,
                                   std::string announcement) override;
    bool dissolveConversation(ConversationId conversation, UserId actor,
                              TimePoint now) override;
    bool updateMemberRole(ConversationId conversation, UserId user,
                          MemberRole role) override;
    bool updateMemberMute(ConversationId conversation, UserId user,
                          std::optional<TimePoint> mutedUntil) override;
    bool removeMember(ConversationId conversation, UserId user) override;
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
    RecallMessageResult recallMessage(MessageId id, UserId actor,
                                      TimePoint now) override;

    FileObject createFile(FileObject file) override;
    std::optional<FileObject> findFile(FileId id) const override;
    bool updateFileProgress(FileId id, UserId owner,
                            std::uint64_t expectedOffset,
                            std::uint64_t uploadedSize) override;
    bool completeFile(FileId id, UserId owner, std::string sha256,
                      TimePoint now) override;
    bool failFile(FileId id, UserId owner) override;
    bool attachFile(MessageId message, FileId file) override;
    std::vector<MessageAttachment> attachmentsForMessage(
        MessageId message) const override;
    bool canAccessFile(UserId user, FileId file) const override;

    OutboxEvent appendOutbox(OutboxEvent event) override;
    std::vector<OutboxEvent> pendingOutbox(std::size_t limit) const override;
    bool markPublished(OutboxEventId id, TimePoint now) override;

    /** 只读运行指标，不把 libpq 类型暴露给业务接口。 */
    [[nodiscard]] PostgresConnectionPool::Snapshot poolSnapshot() const noexcept
    {
        return pool_.snapshot();
    }

    ModelNode createModelNode(ModelNode node) override;
    ModelVersion createModelVersion(ModelVersion version) override;
    ModelEdge createModelEdge(ModelEdge edge) override;
    bool bindAiAccount(ModelBinding binding) override;
    std::optional<ModelNode> findModelNode(ModelNodeId id) const override;
    std::optional<ModelVersion> findModelVersion(ModelVersionId id) const override;
    std::optional<ModelBinding> findModelBinding(UserId aiUser) const override;
    std::vector<ModelEdge> modelEdges() const override;
    bool activateModelVersion(ModelNodeId node, ModelVersionId version,
                              ModelActivationAudit audit) override;
    ModelActivationAudit appendModelActivationAudit(
        ModelActivationAudit audit) override;
    std::vector<ModelActivationAudit> modelActivationAudits(
        ModelNodeId node, std::size_t limit) const override;
    bool upsertGenerationAudit(AiGenerationAudit audit) override;
    std::optional<AiGenerationAudit> findGenerationAudit(
        std::string_view generationId) const override;

    ModelEvaluationSuite createEvaluationSuite(
        ModelEvaluationSuite suite,
        std::vector<ModelEvaluationCase> cases) override;
    std::optional<ModelEvaluationSuite> findEvaluationSuite(
        ModelEvaluationSuiteId id) const override;
    std::vector<ModelEvaluationCase> evaluationCases(
        ModelEvaluationSuiteId suite) const override;
    ModelEvaluationRun createEvaluationRun(ModelEvaluationRun run) override;
    std::optional<ModelEvaluationRun> findEvaluationRun(
        ModelEvaluationRunId id) const override;
    bool appendEvaluationCaseResult(
        ModelEvaluationCaseResult result) override;
    std::vector<ModelEvaluationCaseResult> evaluationCaseResults(
        ModelEvaluationRunId run) const override;
    bool finishEvaluationRun(ModelEvaluationRun run) override;
    bool upsertActivationPolicy(ModelActivationPolicy policy) override;
    std::optional<ModelActivationPolicy> findActivationPolicy(
        ModelNodeId node) const override;
    std::optional<ModelEvaluationRun> latestCompletedEvaluationRun(
        ModelVersionId version, ModelEvaluationSuiteId suite) const override;

    KnowledgeDocument createKnowledgeDocument(
        KnowledgeDocument document,
        std::vector<KnowledgeChunk> chunks) override;
    bool updateKnowledgeDocumentState(
        KnowledgeDocumentId document, KnowledgeDocumentState expected,
        KnowledgeDocumentState next, std::string failureDetail,
        std::optional<TimePoint> indexedAt) override;
    std::optional<KnowledgeDocument> findKnowledgeDocument(
        KnowledgeDocumentId id) const override;
    std::optional<KnowledgeChunk> findKnowledgeChunk(
        KnowledgeChunkId id) const override;
    std::vector<KnowledgeChunk> knowledgeChunks(
        KnowledgeDocumentId document) const override;
    std::vector<KnowledgeDocument> privateKnowledgeDocuments(
        UserId owner) const override;
    std::vector<KnowledgeDocument> conversationKnowledgeDocuments(
        ConversationId conversation) const override;
    std::vector<KnowledgeChunk> allReadyKnowledgeChunks() const override;
    bool saveRagRetrieval(RagRetrieval retrieval,
                          std::vector<RagCitation> citations) override;
    std::optional<RagRetrieval> findRagRetrieval(
        std::string_view generationId) const override;
    std::vector<RagCitation> ragCitations(
        std::string_view generationId) const override;

    TrainingCandidate createTrainingCandidate(TrainingCandidate candidate) override;
    std::optional<TrainingCandidate> findTrainingCandidate(
        TrainingCandidateId id) const override;
    std::optional<TrainingCandidate> findActiveTrainingCandidate(
        UserId owner, MessageId sourceMessage) const override;
    std::vector<TrainingCandidate> trainingCandidatesForOwner(
        UserId owner) const override;
    std::vector<TrainingCandidate> trainingCandidatesByState(
        TrainingCandidateState state) const override;
    bool updateTrainingCandidate(TrainingCandidate candidate) override;
    bool transitionTrainingCandidate(
        TrainingCandidateId id, TrainingCandidateState expected,
        TrainingCandidateState next,
        std::optional<DatasetVersionId> dataset = std::nullopt) override;
    DatasetVersion createDatasetVersion(DatasetVersion version) override;
    std::optional<DatasetVersion> findDatasetVersion(
        DatasetVersionId id) const override;

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
