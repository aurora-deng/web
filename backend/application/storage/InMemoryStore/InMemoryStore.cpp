#include "backend/application/storage/memory/MemoryAccountRepository/MemoryAccountRepository.h"
#include "backend/application/storage/memory/MemoryContactsRepository/MemoryContactsRepository.h"
#include "backend/application/storage/memory/MemoryConversationRepository/MemoryConversationRepository.h"
#include "backend/application/storage/memory/MemoryEvaluationRepository/MemoryEvaluationRepository.h"
#include "backend/application/storage/memory/MemoryFileRepository/MemoryFileRepository.h"
#include "backend/application/storage/memory/MemoryGenerationAuditRepository/MemoryGenerationAuditRepository.h"
#include "backend/application/storage/memory/MemoryKnowledgeRepository/MemoryKnowledgeRepository.h"
#include "backend/application/storage/memory/MemoryMessageWriteRepository/MemoryMessageWriteRepository.h"
#include "backend/application/storage/memory/MemoryModelRepository/MemoryModelRepository.h"
#include "backend/application/storage/memory/MemoryOutboxRepository/MemoryOutboxRepository.h"
#include "backend/application/storage/memory/MemoryTrainingRepository/MemoryTrainingRepository.h"
#include "backend/application/storage/memory/MemoryWorkspaceRepository/MemoryWorkspaceRepository.h"
#include "backend/application/storage/InMemoryStore/InMemoryStore.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace webserver::phase11
{

class InMemoryStore::Transaction final : public ITransaction
{
public:
    explicit Transaction(InMemoryStore &store)
        : store_(store), lock_(store.mutex_), snapshot_(store.state_)
    {
    }

    ~Transaction() override
    {
        if (!finished_)
            rollback();
    }

    void commit() override
    {
        if (finished_)
            throw std::logic_error("transaction already completed");
        finished_ = true;
        lock_.unlock();
    }

    void rollback() override
    {
        if (finished_)
            throw std::logic_error("transaction already completed");
        store_.state_ = std::move(snapshot_);
        finished_ = true;
        lock_.unlock();
    }

private:
    InMemoryStore &store_;
    std::unique_lock<std::recursive_mutex> lock_;
    State snapshot_;
    bool finished_ = false;
};

namespace {
std::string lowerText(std::string value){for(char&c:value)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');return value;}
}
std::vector<webserver::chat::UserCard>InMemoryStore::searchUsers(UserId viewer,std::string_view query,UserId after,std::size_t limit)const
{
    std::lock_guard lock(mutex_);
    return MemoryWorkspaceRepository::searchUsers(state_, std::move(viewer), std::move(query), std::move(after), std::move(limit));
}
std::vector<Message>InMemoryStore::searchVisibleMessages(UserId viewer,ConversationId conversation,std::string_view query,MessageId before,std::size_t limit)const
{
    std::lock_guard lock(mutex_);
    return MemoryWorkspaceRepository::searchVisibleMessages(state_, std::move(viewer), std::move(conversation), std::move(query), std::move(before), std::move(limit));
}
webserver::chat::ConversationPreference InMemoryStore::preference(UserId user,ConversationId conversation)const
{
    std::lock_guard lock(mutex_);
    return MemoryWorkspaceRepository::preference(state_, std::move(user), std::move(conversation));
}
webserver::chat::PreferenceWrite InMemoryStore::writePreference(UserId user,ConversationId conversation,bool muted)
{
    std::lock_guard lock(mutex_);
    return MemoryWorkspaceRepository::writePreference(state_, std::move(user), std::move(conversation), std::move(muted));
}

std::unique_ptr<ITransaction> InMemoryStore::beginTransaction()
{
    return std::make_unique<Transaction>(*this);
}



User InMemoryStore::create(User user)
{
    std::lock_guard lock(mutex_);
    return MemoryAccountRepository::create(state_, std::move(user));
}

std::optional<User> InMemoryStore::findUser(UserId id) const
{
    std::lock_guard lock(mutex_);
    return MemoryAccountRepository::findUser(state_, std::move(id));
}

std::optional<User> InMemoryStore::findUserByName(std::string_view username) const
{
    std::lock_guard lock(mutex_);
    return MemoryAccountRepository::findUserByName(state_, std::move(username));
}

bool InMemoryStore::updateProfile(UserId id, std::string displayName,
                                  std::string biography)
{
    std::lock_guard lock(mutex_);
    return MemoryAccountRepository::updateProfile(state_, std::move(id), std::move(displayName), std::move(biography));
}

// 收据随State一起快照与回滚，契约与PostgreSQL一致。
ProfileWriteResult InMemoryStore::writeProfileOnce(UserId id,std::string operation,std::string displayName,std::string biography)
{
    std::lock_guard lock(mutex_);
    return MemoryAccountRepository::writeProfileOnce(state_, std::move(id), std::move(operation), std::move(displayName), std::move(biography));
}
bool InMemoryStore::updateAvatar(UserId id, std::optional<FileId> avatar)
{
    std::lock_guard lock(mutex_);
    return MemoryAccountRepository::updateAvatar(state_, std::move(id), std::move(avatar));
}

Session InMemoryStore::createSession(Session session)
{
    std::lock_guard lock(mutex_);
    return MemoryAccountRepository::createSession(state_, std::move(session));
}

std::optional<Session> InMemoryStore::findSessionByTokenHash(
    std::string_view tokenHash) const
{
    std::lock_guard lock(mutex_);
    return MemoryAccountRepository::findSessionByTokenHash(state_, std::move(tokenHash));
}

bool InMemoryStore::revokeSession(std::string_view sessionId, TimePoint now)
{
    std::lock_guard lock(mutex_);
    return MemoryAccountRepository::revokeSession(state_, std::move(sessionId), std::move(now));
}

FriendRequest InMemoryStore::createFriendRequest(FriendRequest request)
{
    std::lock_guard lock(mutex_);
    return MemoryContactsRepository::createFriendRequest(state_, std::move(request));
}

std::optional<FriendRequest> InMemoryStore::findFriendRequest(
    FriendRequestId id) const
{
    std::lock_guard lock(mutex_);
    return MemoryContactsRepository::findFriendRequest(state_, std::move(id));
}

bool InMemoryStore::decideFriendRequest(FriendRequestId id, UserId receiver,
                                        FriendRequestState decision, TimePoint now)
{
    std::lock_guard lock(mutex_);
    return MemoryContactsRepository::decideFriendRequest(state_, std::move(id), std::move(receiver), std::move(decision), std::move(now));
}

bool InMemoryStore::areFriends(UserId first, UserId second) const
{
    std::lock_guard lock(mutex_);
    return MemoryContactsRepository::areFriends(state_, std::move(first), std::move(second));
}

std::vector<UserId> InMemoryStore::listFriendIds(UserId user) const
{
    std::lock_guard lock(mutex_);
    return MemoryContactsRepository::listFriendIds(state_, std::move(user));
}

std::vector<FriendRequest> InMemoryStore::pendingRequests(UserId receiver) const
{
    std::lock_guard lock(mutex_);
    return MemoryContactsRepository::pendingRequests(state_, std::move(receiver));
}

bool InMemoryStore::removeFriendship(UserId first, UserId second)
{
    std::lock_guard lock(mutex_);
    return MemoryContactsRepository::removeFriendship(state_, std::move(first), std::move(second));
}

ContactSettings InMemoryStore::contactSettings(UserId owner,
                                                UserId contact) const
{
    std::lock_guard lock(mutex_);
    return MemoryContactsRepository::contactSettings(state_, std::move(owner), std::move(contact));
}

bool InMemoryStore::updateContactSettings(UserId owner, UserId contact,
                                          std::string remark,
                                          bool notificationsMuted)
{
    std::lock_guard lock(mutex_);
    return MemoryContactsRepository::updateContactSettings(state_, std::move(owner), std::move(contact), std::move(remark), std::move(notificationsMuted));
}

bool InMemoryStore::blockUser(UserId blocker, UserId blocked, TimePoint now)
{
    std::lock_guard lock(mutex_);
    return MemoryContactsRepository::blockUser(state_, std::move(blocker), std::move(blocked), std::move(now));
}

bool InMemoryStore::unblockUser(UserId blocker, UserId blocked)
{
    std::lock_guard lock(mutex_);
    return MemoryContactsRepository::unblockUser(state_, std::move(blocker), std::move(blocked));
}

bool InMemoryStore::isBlocked(UserId blocker, UserId blocked) const
{
    std::lock_guard lock(mutex_);
    return MemoryContactsRepository::isBlocked(state_, std::move(blocker), std::move(blocked));
}

bool InMemoryStore::blockedEitherDirection(UserId first, UserId second) const
{
    std::lock_guard lock(mutex_);
    return MemoryContactsRepository::blockedEitherDirection(state_, std::move(first), std::move(second));
}

std::vector<UserId> InMemoryStore::listBlockedIds(UserId blocker) const
{
    std::lock_guard lock(mutex_);
    return MemoryContactsRepository::listBlockedIds(state_, std::move(blocker));
}

bool InMemoryStore::updateLastActive(UserId user, TimePoint now)
{
    std::lock_guard lock(mutex_);
    return MemoryContactsRepository::updateLastActive(state_, std::move(user), std::move(now));
}

std::optional<TimePoint> InMemoryStore::lastActive(UserId user) const
{
    std::lock_guard lock(mutex_);
    return MemoryContactsRepository::lastActive(state_, std::move(user));
}

Conversation InMemoryStore::createConversation(
    Conversation conversation, std::vector<ConversationMember> members)
{
    std::lock_guard lock(mutex_);
    return MemoryConversationRepository::createConversation(state_, std::move(conversation), std::move(members));
}

std::optional<Conversation> InMemoryStore::findConversation(
    ConversationId id) const
{
    std::lock_guard lock(mutex_);
    return MemoryConversationRepository::findConversation(state_, std::move(id));
}

std::optional<ConversationMember> InMemoryStore::findMember(
    ConversationId conversation, UserId user) const
{
    std::lock_guard lock(mutex_);
    return MemoryConversationRepository::findMember(state_, std::move(conversation), std::move(user));
}

std::optional<Conversation> InMemoryStore::findDirectConversation(
    UserId first, UserId second) const
{
    std::lock_guard lock(mutex_);
    return MemoryConversationRepository::findDirectConversation(state_, std::move(first), std::move(second));
}

std::vector<Conversation> InMemoryStore::listConversations(UserId user) const
{
    std::lock_guard lock(mutex_);
    return MemoryConversationRepository::listConversations(state_, std::move(user));
}

std::vector<ConversationSummary> InMemoryStore::listConversationSummaries(
    UserId user) const
{
    std::lock_guard lock(mutex_);
    return MemoryConversationRepository::listConversationSummaries(state_, std::move(user));
}

std::vector<ConversationMember> InMemoryStore::listMembers(
    ConversationId conversation) const
{
    std::lock_guard lock(mutex_);
    return MemoryConversationRepository::listMembers(state_, std::move(conversation));
}

bool InMemoryStore::addMember(ConversationMember member)
{
    std::lock_guard lock(mutex_);
    return MemoryConversationRepository::addMember(state_, std::move(member));
}

bool InMemoryStore::updateConversationProfile(
    ConversationId conversation, std::string title,
    std::string announcement)
{
    std::lock_guard lock(mutex_);
    return MemoryConversationRepository::updateConversationProfile(state_, std::move(conversation), std::move(title), std::move(announcement));
}

bool InMemoryStore::dissolveConversation(ConversationId conversation,
                                         UserId actor, TimePoint now)
{
    std::lock_guard lock(mutex_);
    return MemoryConversationRepository::dissolveConversation(state_, std::move(conversation), std::move(actor), std::move(now));
}

bool InMemoryStore::updateMemberRole(ConversationId conversation, UserId user,
                                     MemberRole role)
{
    std::lock_guard lock(mutex_);
    return MemoryConversationRepository::updateMemberRole(state_, std::move(conversation), std::move(user), std::move(role));
}

bool InMemoryStore::updateMemberMute(
    ConversationId conversation, UserId user,
    std::optional<TimePoint> mutedUntil)
{
    std::lock_guard lock(mutex_);
    return MemoryConversationRepository::updateMemberMute(state_, std::move(conversation), std::move(user), std::move(mutedUntil));
}

bool InMemoryStore::removeMember(ConversationId conversation, UserId user)
{
    std::lock_guard lock(mutex_);
    return MemoryConversationRepository::removeMember(state_, std::move(conversation), std::move(user));
}

bool InMemoryStore::updateDelivered(ConversationId conversation, UserId user,
                                    std::uint64_t sequence)
{
    std::lock_guard lock(mutex_);
    return MemoryConversationRepository::updateDelivered(state_, std::move(conversation), std::move(user), std::move(sequence));
}

bool InMemoryStore::updateRead(ConversationId conversation, UserId user,
                               std::uint64_t sequence)
{
    std::lock_guard lock(mutex_);
    return MemoryConversationRepository::updateRead(state_, std::move(conversation), std::move(user), std::move(sequence));
}

AppendMessageResult InMemoryStore::appendMessage(Message message)
{
    std::lock_guard lock(mutex_);
    return MemoryMessageWriteRepository::appendMessage(state_, std::move(message));
}

Message InMemoryStore::hydrateMessageLocked(Message message) const
{
    std::lock_guard lock(mutex_);
    return MemoryMessageWriteRepository::hydrateMessageLocked(state_, std::move(message));
}

std::vector<Message> InMemoryStore::messagesAfter(
    ConversationId conversation, std::uint64_t sequence,
    std::size_t limit) const
{
    std::lock_guard lock(mutex_);
    return MemoryMessageWriteRepository::messagesAfter(state_, std::move(conversation), std::move(sequence), std::move(limit));
}

std::vector<Message> InMemoryStore::messagesBefore(
    ConversationId conversation, std::uint64_t before, std::size_t limit) const
{
    std::lock_guard lock(mutex_);
    return MemoryMessageWriteRepository::messagesBefore(state_, std::move(conversation), std::move(before), std::move(limit));
}

std::optional<Message> InMemoryStore::findOwnMessage(
    ConversationId conversation, UserId sender, std::string_view clientMessageId) const
{
    std::lock_guard lock(mutex_);
    return MemoryMessageWriteRepository::findOwnMessage(state_, std::move(conversation), std::move(sender), std::move(clientMessageId));
}

std::vector<Message> InMemoryStore::searchMessages(
    ConversationId conversation, std::string_view query,
    std::size_t limit) const
{
    std::lock_guard lock(mutex_);
    return MemoryMessageWriteRepository::searchMessages(state_, std::move(conversation), std::move(query), std::move(limit));
}

std::optional<Message> InMemoryStore::findMessage(MessageId id) const
{
    std::lock_guard lock(mutex_);
    return MemoryMessageWriteRepository::findMessage(state_, std::move(id));
}

RecallMessageResult InMemoryStore::recallMessage(MessageId id, UserId actor,
                                                 TimePoint now)
{
    std::lock_guard lock(mutex_);
    return MemoryMessageWriteRepository::recallMessage(state_, std::move(id), std::move(actor), std::move(now));
}

FileObject InMemoryStore::createFile(FileObject file)
{
    std::lock_guard lock(mutex_);
    return MemoryFileRepository::createFile(state_, std::move(file));
}

std::optional<FileObject> InMemoryStore::findFile(FileId id) const
{
    std::lock_guard lock(mutex_);
    return MemoryFileRepository::findFile(state_, std::move(id));
}

bool InMemoryStore::updateFileProgress(FileId id, UserId owner,
                                       std::uint64_t expectedOffset,
                                       std::uint64_t uploadedSize)
{
    std::lock_guard lock(mutex_);
    return MemoryFileRepository::updateFileProgress(state_, std::move(id), std::move(owner), std::move(expectedOffset), std::move(uploadedSize));
}

bool InMemoryStore::completeFile(FileId id, UserId owner,
                                 std::string sha256, TimePoint now)
{
    std::lock_guard lock(mutex_);
    return MemoryFileRepository::completeFile(state_, std::move(id), std::move(owner), std::move(sha256), std::move(now));
}

bool InMemoryStore::failFile(FileId id, UserId owner)
{
    std::lock_guard lock(mutex_);
    return MemoryFileRepository::failFile(state_, std::move(id), std::move(owner));
}

bool InMemoryStore::attachFile(MessageId message, FileId file)
{
    std::lock_guard lock(mutex_);
    return MemoryFileRepository::attachFile(state_, std::move(message), std::move(file));
}

std::vector<MessageAttachment> InMemoryStore::attachmentsForMessage(
    MessageId message) const
{
    std::lock_guard lock(mutex_);
    return MemoryFileRepository::attachmentsForMessage(state_, std::move(message));
}

bool InMemoryStore::canAccessFile(UserId user, FileId file) const
{
    std::lock_guard lock(mutex_);
    return MemoryFileRepository::canAccessFile(state_, std::move(user), std::move(file));
}

OutboxEvent InMemoryStore::appendOutbox(OutboxEvent event)
{
    std::lock_guard lock(mutex_);
    return MemoryOutboxRepository::appendOutbox(state_, std::move(event));
}

std::vector<OutboxEvent> InMemoryStore::pendingOutbox(std::size_t limit) const
{
    std::lock_guard lock(mutex_);
    return MemoryOutboxRepository::pendingOutbox(state_, std::move(limit));
}

bool InMemoryStore::markPublished(OutboxEventId id, TimePoint now)
{
    std::lock_guard lock(mutex_);
    return MemoryOutboxRepository::markPublished(state_, std::move(id), std::move(now));
}

ModelNode InMemoryStore::createModelNode(ModelNode node)
{
    std::lock_guard lock(mutex_);
    return MemoryModelRepository::createModelNode(state_, std::move(node));
}

ModelVersion InMemoryStore::createModelVersion(ModelVersion version)
{
    std::lock_guard lock(mutex_);
    return MemoryModelRepository::createModelVersion(state_, std::move(version));
}

std::pair<ModelVersion, ModelTrainingLineage>
InMemoryStore::createTrainedModelVersion(ModelVersion version,
                                         ModelTrainingLineage lineage)
{
    std::lock_guard lock(mutex_);
    return MemoryModelRepository::createTrainedModelVersion(state_, std::move(version), std::move(lineage));
}

ModelEdge InMemoryStore::createModelEdge(ModelEdge edge)
{
    std::lock_guard lock(mutex_);
    return MemoryModelRepository::createModelEdge(state_, std::move(edge));
}

bool InMemoryStore::bindAiAccount(ModelBinding binding)
{
    std::lock_guard lock(mutex_);
    return MemoryModelRepository::bindAiAccount(state_, std::move(binding));
}

std::optional<ModelNode> InMemoryStore::findModelNode(ModelNodeId id) const
{
    std::lock_guard lock(mutex_);
    return MemoryModelRepository::findModelNode(state_, std::move(id));
}

std::vector<ModelNode> InMemoryStore::modelNodes(ModelNodeId after,std::size_t limit)const
{
    std::lock_guard lock(mutex_);
    return MemoryModelRepository::modelNodes(state_, std::move(after), std::move(limit));
}
std::vector<ModelVersion> InMemoryStore::modelVersions(ModelNodeId node,ModelVersionId after,std::size_t limit)const
{
    std::lock_guard lock(mutex_);
    return MemoryModelRepository::modelVersions(state_, std::move(node), std::move(after), std::move(limit));
}

std::optional<ModelVersion> InMemoryStore::findModelVersion(
    ModelVersionId id) const
{
    std::lock_guard lock(mutex_);
    return MemoryModelRepository::findModelVersion(state_, std::move(id));
}

std::optional<ModelTrainingLineage>
InMemoryStore::findModelTrainingLineage(ModelVersionId version) const
{
    std::lock_guard lock(mutex_);
    return MemoryModelRepository::findModelTrainingLineage(state_, std::move(version));
}

std::optional<ModelBinding> InMemoryStore::findModelBinding(UserId aiUser) const
{
    std::lock_guard lock(mutex_);
    return MemoryModelRepository::findModelBinding(state_, std::move(aiUser));
}

std::vector<ModelEdge> InMemoryStore::modelEdges() const
{
    std::lock_guard lock(mutex_);
    return MemoryModelRepository::modelEdges(state_);
}

bool InMemoryStore::activateModelVersion(ModelNodeId node, ModelVersionId version,
                                         ModelActivationAudit audit)
{
    std::lock_guard lock(mutex_);
    return MemoryModelRepository::activateModelVersion(state_, std::move(node), std::move(version), std::move(audit));
}

ModelActivationAudit InMemoryStore::appendModelActivationAudit(
    ModelActivationAudit audit)
{
    std::lock_guard lock(mutex_);
    return MemoryModelRepository::appendModelActivationAudit(state_, std::move(audit));
}

std::vector<ModelActivationAudit> InMemoryStore::modelActivationAudits(
    ModelNodeId node, std::size_t limit) const
{
    std::lock_guard lock(mutex_);
    return MemoryModelRepository::modelActivationAudits(state_, std::move(node), std::move(limit));
}

bool InMemoryStore::upsertGenerationAudit(AiGenerationAudit audit)
{
    std::lock_guard lock(mutex_);
    return MemoryGenerationAuditRepository::upsertGenerationAudit(state_, std::move(audit));
}

std::optional<AiGenerationAudit> InMemoryStore::findGenerationAudit(
    std::string_view generationId) const
{
    std::lock_guard lock(mutex_);
    return MemoryGenerationAuditRepository::findGenerationAudit(state_, std::move(generationId));
}

ModelEvaluationSuite InMemoryStore::createEvaluationSuite(
    ModelEvaluationSuite suite, std::vector<ModelEvaluationCase> cases)
{
    std::lock_guard lock(mutex_);
    return MemoryEvaluationRepository::createEvaluationSuite(state_, std::move(suite), std::move(cases));
}

std::optional<ModelEvaluationSuite> InMemoryStore::findEvaluationSuite(
    ModelEvaluationSuiteId id) const
{
    std::lock_guard lock(mutex_);
    return MemoryEvaluationRepository::findEvaluationSuite(state_, std::move(id));
}

std::vector<ModelEvaluationCase> InMemoryStore::evaluationCases(
    ModelEvaluationSuiteId suite) const
{
    std::lock_guard lock(mutex_);
    return MemoryEvaluationRepository::evaluationCases(state_, std::move(suite));
}

ModelEvaluationRun InMemoryStore::createEvaluationRun(ModelEvaluationRun run)
{
    std::lock_guard lock(mutex_);
    return MemoryEvaluationRepository::createEvaluationRun(state_, std::move(run));
}

std::optional<ModelEvaluationRun> InMemoryStore::findEvaluationRun(
    ModelEvaluationRunId id) const
{
    std::lock_guard lock(mutex_);
    return MemoryEvaluationRepository::findEvaluationRun(state_, std::move(id));
}

bool InMemoryStore::appendEvaluationCaseResult(
    ModelEvaluationCaseResult result)
{
    std::lock_guard lock(mutex_);
    return MemoryEvaluationRepository::appendEvaluationCaseResult(state_, std::move(result));
}

std::vector<ModelEvaluationCaseResult> InMemoryStore::evaluationCaseResults(
    ModelEvaluationRunId run) const
{
    std::lock_guard lock(mutex_);
    return MemoryEvaluationRepository::evaluationCaseResults(state_, std::move(run));
}

bool InMemoryStore::finishEvaluationRun(ModelEvaluationRun run)
{
    std::lock_guard lock(mutex_);
    return MemoryEvaluationRepository::finishEvaluationRun(state_, std::move(run));
}

bool InMemoryStore::upsertActivationPolicy(ModelActivationPolicy policy)
{
    std::lock_guard lock(mutex_);
    return MemoryEvaluationRepository::upsertActivationPolicy(state_, std::move(policy));
}

std::optional<ModelActivationPolicy> InMemoryStore::findActivationPolicy(
    ModelNodeId node) const
{
    std::lock_guard lock(mutex_);
    return MemoryEvaluationRepository::findActivationPolicy(state_, std::move(node));
}

std::optional<ModelEvaluationRun>
InMemoryStore::latestCompletedEvaluationRun(
    ModelVersionId version, ModelEvaluationSuiteId suite) const
{
    std::lock_guard lock(mutex_);
    return MemoryEvaluationRepository::latestCompletedEvaluationRun(state_, std::move(version), std::move(suite));
}

KnowledgeDocument InMemoryStore::createKnowledgeDocument(
    KnowledgeDocument document, std::vector<KnowledgeChunk> chunks)
{
    std::lock_guard lock(mutex_);
    return MemoryKnowledgeRepository::createKnowledgeDocument(state_, std::move(document), std::move(chunks));
}

bool InMemoryStore::updateKnowledgeDocumentState(
    KnowledgeDocumentId document, KnowledgeDocumentState expected,
    KnowledgeDocumentState next, std::string failureDetail,
    std::optional<TimePoint> indexedAt)
{
    std::lock_guard lock(mutex_);
    return MemoryKnowledgeRepository::updateKnowledgeDocumentState(state_, std::move(document), std::move(expected), std::move(next), std::move(failureDetail), std::move(indexedAt));
}

std::optional<KnowledgeDocument> InMemoryStore::findKnowledgeDocument(
    KnowledgeDocumentId id) const
{
    std::lock_guard lock(mutex_);
    return MemoryKnowledgeRepository::findKnowledgeDocument(state_, std::move(id));
}

std::optional<KnowledgeChunk> InMemoryStore::findKnowledgeChunk(
    KnowledgeChunkId id) const
{
    std::lock_guard lock(mutex_);
    return MemoryKnowledgeRepository::findKnowledgeChunk(state_, std::move(id));
}

std::vector<KnowledgeChunk> InMemoryStore::knowledgeChunks(
    KnowledgeDocumentId document) const
{
    std::lock_guard lock(mutex_);
    return MemoryKnowledgeRepository::knowledgeChunks(state_, std::move(document));
}

std::vector<KnowledgeDocument> InMemoryStore::privateKnowledgeDocuments(
    UserId owner) const
{
    std::lock_guard lock(mutex_);
    return MemoryKnowledgeRepository::privateKnowledgeDocuments(state_, std::move(owner));
}

std::vector<KnowledgeDocument>
InMemoryStore::conversationKnowledgeDocuments(
    ConversationId conversation) const
{
    std::lock_guard lock(mutex_);
    return MemoryKnowledgeRepository::conversationKnowledgeDocuments(state_, std::move(conversation));
}

std::vector<KnowledgeChunk> InMemoryStore::allReadyKnowledgeChunks() const
{
    std::lock_guard lock(mutex_);
    return MemoryKnowledgeRepository::allReadyKnowledgeChunks(state_);
}

bool InMemoryStore::saveRagRetrieval(
    RagRetrieval retrieval, std::vector<RagCitation> citations)
{
    std::lock_guard lock(mutex_);
    return MemoryKnowledgeRepository::saveRagRetrieval(state_, std::move(retrieval), std::move(citations));
}

std::optional<RagRetrieval> InMemoryStore::findRagRetrieval(
    std::string_view generationId) const
{
    std::lock_guard lock(mutex_);
    return MemoryKnowledgeRepository::findRagRetrieval(state_, std::move(generationId));
}

std::vector<RagCitation> InMemoryStore::ragCitations(
    std::string_view generationId) const
{
    std::lock_guard lock(mutex_);
    return MemoryKnowledgeRepository::ragCitations(state_, std::move(generationId));
}

TrainingCandidate InMemoryStore::createTrainingCandidate(
    TrainingCandidate candidate)
{
    std::lock_guard lock(mutex_);
    return MemoryTrainingRepository::createTrainingCandidate(state_, std::move(candidate));
}

std::optional<TrainingCandidate> InMemoryStore::findTrainingCandidate(
    TrainingCandidateId id) const
{
    std::lock_guard lock(mutex_);
    return MemoryTrainingRepository::findTrainingCandidate(state_, std::move(id));
}

std::optional<TrainingCandidate> InMemoryStore::findActiveTrainingCandidate(
    UserId owner, MessageId sourceMessage) const
{
    std::lock_guard lock(mutex_);
    return MemoryTrainingRepository::findActiveTrainingCandidate(state_, std::move(owner), std::move(sourceMessage));
}

std::vector<TrainingCandidate> InMemoryStore::trainingCandidatesForOwner(
    UserId owner) const
{
    std::lock_guard lock(mutex_);
    return MemoryTrainingRepository::trainingCandidatesForOwner(state_, std::move(owner));
}

std::vector<TrainingCandidate> InMemoryStore::trainingCandidatesByState(
    TrainingCandidateState state) const
{
    std::lock_guard lock(mutex_);
    return MemoryTrainingRepository::trainingCandidatesByState(state_, std::move(state));
}

bool InMemoryStore::updateTrainingCandidate(TrainingCandidate candidate)
{
    std::lock_guard lock(mutex_);
    return MemoryTrainingRepository::updateTrainingCandidate(state_, std::move(candidate));
}

bool InMemoryStore::transitionTrainingCandidate(
    TrainingCandidateId id, TrainingCandidateState expected,
    TrainingCandidateState next, std::optional<DatasetVersionId> dataset)
{
    std::lock_guard lock(mutex_);
    return MemoryTrainingRepository::transitionTrainingCandidate(state_, std::move(id), std::move(expected), std::move(next), std::move(dataset));
}

DatasetVersion InMemoryStore::createDatasetVersion(DatasetVersion version)
{
    std::lock_guard lock(mutex_);
    return MemoryTrainingRepository::createDatasetVersion(state_, std::move(version));
}

std::optional<DatasetVersion> InMemoryStore::findDatasetVersion(
    DatasetVersionId id) const
{
    std::lock_guard lock(mutex_);
    return MemoryTrainingRepository::findDatasetVersion(state_, std::move(id));
}

} // namespace webserver::phase11

namespace webserver::phase11 {
bool InMemoryStore::markPublishedBatch(const std::vector<OutboxEventId> &ids,TimePoint now) {
    std::lock_guard lock(mutex_);
    return MemoryOutboxRepository::markPublishedBatch(state_,ids,now);
}
}
