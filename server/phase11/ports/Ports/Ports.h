#pragma once

#include "server/phase11/domain/Domain/Domain.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
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
    virtual bool updateAvatar(UserId id, std::optional<FileId> avatar) = 0;
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
    virtual bool removeFriendship(UserId first, UserId second) = 0;
    [[nodiscard]] virtual ContactSettings contactSettings(
        UserId owner, UserId contact) const = 0;
    virtual bool updateContactSettings(UserId owner, UserId contact,
                                       std::string remark,
                                       bool notificationsMuted) = 0;
    virtual bool blockUser(UserId blocker, UserId blocked,
                           TimePoint now) = 0;
    virtual bool unblockUser(UserId blocker, UserId blocked) = 0;
    [[nodiscard]] virtual bool isBlocked(UserId blocker,
                                         UserId blocked) const = 0;
    [[nodiscard]] virtual bool blockedEitherDirection(UserId first,
                                                       UserId second) const = 0;
    [[nodiscard]] virtual std::vector<UserId> listBlockedIds(
        UserId blocker) const = 0;
    virtual bool updateLastActive(UserId user, TimePoint now) = 0;
    [[nodiscard]] virtual std::optional<TimePoint> lastActive(
        UserId user) const = 0;
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
    [[nodiscard]] virtual std::vector<ConversationSummary>
    listConversationSummaries(UserId user) const = 0;
    [[nodiscard]] virtual std::vector<ConversationMember> listMembers(
        ConversationId conversation) const = 0;
    virtual bool addMember(ConversationMember member) = 0;
    virtual bool updateConversationProfile(ConversationId conversation,
                                           std::string title,
                                           std::string announcement) = 0;
    virtual bool dissolveConversation(ConversationId conversation,
                                      UserId actor, TimePoint now) = 0;
    virtual bool updateMemberRole(ConversationId conversation, UserId user,
                                  MemberRole role) = 0;
    virtual bool updateMemberMute(ConversationId conversation, UserId user,
                                  std::optional<TimePoint> mutedUntil) = 0;
    virtual bool removeMember(ConversationId conversation, UserId user) = 0;
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
    virtual RecallMessageResult recallMessage(MessageId id, UserId actor,
                                              TimePoint now) = 0;
};

class IFileRepository
{
public:
    virtual ~IFileRepository() = default;
    virtual FileObject createFile(FileObject file) = 0;
    [[nodiscard]] virtual std::optional<FileObject> findFile(FileId id) const = 0;
    virtual bool updateFileProgress(FileId id, UserId owner,
                                    std::uint64_t expectedOffset,
                                    std::uint64_t uploadedSize) = 0;
    virtual bool completeFile(FileId id, UserId owner, std::string sha256,
                              TimePoint now) = 0;
    virtual bool failFile(FileId id, UserId owner) = 0;
    virtual bool attachFile(MessageId message, FileId file) = 0;
    [[nodiscard]] virtual std::vector<MessageAttachment> attachmentsForMessage(
        MessageId message) const = 0;
    [[nodiscard]] virtual bool canAccessFile(UserId user, FileId file) const = 0;
};

/**
 * 文件字节仓库端口。业务层只知道“准备、追加、封存、取路径”，不知道本地磁盘
 * 如何分目录；以后换成对象存储时不需要改聊天规则。
 */
class IFileStorage
{
public:
    virtual ~IFileStorage() = default;
    virtual void prepare(FileId file) = 0;
    [[nodiscard]] virtual std::uint64_t pendingSize(FileId file) const = 0;
    virtual std::uint64_t append(FileId file, std::uint64_t offset,
                                 std::string_view bytes) = 0;
    virtual StoredFileSummary finalize(FileId file) = 0;
    [[nodiscard]] virtual std::string readyPath(FileId file) const = 0;
    virtual void remove(FileId file) noexcept = 0;
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
    /** 成功切换和 succeeded 审计必须在同一个存储原子操作内完成。 */
    virtual bool activateModelVersion(ModelNodeId node, ModelVersionId version,
                                      ModelActivationAudit audit) = 0;
    virtual ModelActivationAudit appendModelActivationAudit(
        ModelActivationAudit audit) = 0;
    [[nodiscard]] virtual std::vector<ModelActivationAudit>
    modelActivationAudits(ModelNodeId node, std::size_t limit) const = 0;
};

class IAiGenerationAuditRepository
{
public:
    virtual ~IAiGenerationAuditRepository() = default;
    /** 同一 generationId 只接受 revision 不小于现值的快照。 */
    virtual bool upsertGenerationAudit(AiGenerationAudit audit) = 0;
    [[nodiscard]] virtual std::optional<AiGenerationAudit>
    findGenerationAudit(std::string_view generationId) const = 0;
};

class IModelEvaluationRepository
{
public:
    virtual ~IModelEvaluationRepository() = default;
    /** 试卷和题目必须一起落库，不能留下没有题目的半成品评测集。 */
    virtual ModelEvaluationSuite createEvaluationSuite(
        ModelEvaluationSuite suite,
        std::vector<ModelEvaluationCase> cases) = 0;
    [[nodiscard]] virtual std::optional<ModelEvaluationSuite>
    findEvaluationSuite(ModelEvaluationSuiteId id) const = 0;
    [[nodiscard]] virtual std::vector<ModelEvaluationCase> evaluationCases(
        ModelEvaluationSuiteId suite) const = 0;
    virtual ModelEvaluationRun createEvaluationRun(ModelEvaluationRun run) = 0;
    [[nodiscard]] virtual std::optional<ModelEvaluationRun> findEvaluationRun(
        ModelEvaluationRunId id) const = 0;
    /** 同一运行中的同一道题只能写入一次结果。 */
    virtual bool appendEvaluationCaseResult(
        ModelEvaluationCaseResult result) = 0;
    [[nodiscard]] virtual std::vector<ModelEvaluationCaseResult>
    evaluationCaseResults(ModelEvaluationRunId run) const = 0;
    /** 只允许把 running 原子推进到 completed/aborted。 */
    virtual bool finishEvaluationRun(ModelEvaluationRun run) = 0;
    virtual bool upsertActivationPolicy(ModelActivationPolicy policy) = 0;
    [[nodiscard]] virtual std::optional<ModelActivationPolicy>
    findActivationPolicy(ModelNodeId node) const = 0;
    [[nodiscard]] virtual std::optional<ModelEvaluationRun>
    latestCompletedEvaluationRun(ModelVersionId version,
                                 ModelEvaluationSuiteId suite) const = 0;
};

/** ModelRegistry 只问“能否上岗”，不需要知道评测表如何存储或计分。 */
class IModelQualityGate
{
public:
    virtual ~IModelQualityGate() = default;
    [[nodiscard]] virtual ModelQualityGateDecision assess(
        ModelNodeId node, ModelVersionId version, TimePoint now) const = 0;
};

class IKnowledgeRepository
{
public:
    virtual ~IKnowledgeRepository() = default;
    /** 文档登记卡与全部 Chunk 必须原子写入，不能留下半份知识。 */
    virtual KnowledgeDocument createKnowledgeDocument(
        KnowledgeDocument document, std::vector<KnowledgeChunk> chunks) = 0;
    virtual bool updateKnowledgeDocumentState(
        KnowledgeDocumentId document, KnowledgeDocumentState expected,
        KnowledgeDocumentState next, std::string failureDetail,
        std::optional<TimePoint> indexedAt) = 0;
    [[nodiscard]] virtual std::optional<KnowledgeDocument>
    findKnowledgeDocument(KnowledgeDocumentId id) const = 0;
    [[nodiscard]] virtual std::optional<KnowledgeChunk>
    findKnowledgeChunk(KnowledgeChunkId id) const = 0;
    [[nodiscard]] virtual std::vector<KnowledgeChunk> knowledgeChunks(
        KnowledgeDocumentId document) const = 0;
    [[nodiscard]] virtual std::vector<KnowledgeDocument>
    privateKnowledgeDocuments(UserId owner) const = 0;
    [[nodiscard]] virtual std::vector<KnowledgeDocument>
    conversationKnowledgeDocuments(ConversationId conversation) const = 0;
    [[nodiscard]] virtual std::vector<KnowledgeChunk>
    allReadyKnowledgeChunks() const = 0;
    /** Retrieval 与有序 Citation 一起保存，generationId 只允许写入一次。 */
    virtual bool saveRagRetrieval(RagRetrieval retrieval,
                                  std::vector<RagCitation> citations) = 0;
    [[nodiscard]] virtual std::optional<RagRetrieval> findRagRetrieval(
        std::string_view generationId) const = 0;
    [[nodiscard]] virtual std::vector<RagCitation> ragCitations(
        std::string_view generationId) const = 0;
};

/** 可替换检索设备；Phase 21 的参考实现是无依赖词法索引，后续可换向量库。 */
class IKnowledgeIndex
{
public:
    virtual ~IKnowledgeIndex() = default;
    virtual void replaceAll(std::vector<KnowledgeChunk> chunks) = 0;
    virtual void upsert(std::vector<KnowledgeChunk> chunks) = 0;
    virtual void removeDocument(KnowledgeDocumentId document) = 0;
    [[nodiscard]] virtual std::vector<KnowledgeSearchHit> search(
        std::string_view query,
        const std::vector<KnowledgeDocumentId> &allowedDocuments,
        std::size_t limit) const = 0;
};

/** AiChatService 只消费上下文，不知道切片、索引和权限如何实现。 */
class IRagContextProvider
{
public:
    virtual ~IRagContextProvider() = default;
    virtual RagContext retrieveForGeneration(
        std::string generationId, UserId requester,
        ConversationId conversation, std::string_view query,
        std::size_t limit, TimePoint now) = 0;
};

class ITrainingRepository
{
public:
    virtual ~ITrainingRepository() = default;
    virtual TrainingCandidate createTrainingCandidate(TrainingCandidate candidate) = 0;
    [[nodiscard]] virtual std::optional<TrainingCandidate> findTrainingCandidate(
        TrainingCandidateId id) const = 0;
    [[nodiscard]] virtual std::optional<TrainingCandidate>
    findActiveTrainingCandidate(UserId owner, MessageId sourceMessage) const = 0;
    [[nodiscard]] virtual std::vector<TrainingCandidate>
    trainingCandidatesForOwner(UserId owner) const = 0;
    [[nodiscard]] virtual std::vector<TrainingCandidate>
    trainingCandidatesByState(TrainingCandidateState state) const = 0;
    virtual bool updateTrainingCandidate(TrainingCandidate candidate) = 0;
    virtual bool transitionTrainingCandidate(
        TrainingCandidateId id, TrainingCandidateState expected,
        TrainingCandidateState next,
        std::optional<DatasetVersionId> dataset = std::nullopt) = 0;
    virtual DatasetVersion createDatasetVersion(DatasetVersion version) = 0;
    [[nodiscard]] virtual std::optional<DatasetVersion> findDatasetVersion(
        DatasetVersionId id) const = 0;
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
