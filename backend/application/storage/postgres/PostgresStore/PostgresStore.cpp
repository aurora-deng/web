#include "backend/application/storage/postgres/PostgresValueCodec/PostgresValueCodec.h"
#include "backend/application/storage/postgres/PostgresAccountRepository/PostgresAccountRepository.h"
#include "backend/application/storage/postgres/PostgresContactsRepository/PostgresContactsRepository.h"
#include "backend/application/storage/postgres/PostgresConversationRepository/PostgresConversationRepository.h"
#include "backend/application/storage/postgres/PostgresEvaluationRepository/PostgresEvaluationRepository.h"
#include "backend/application/storage/postgres/PostgresFileRepository/PostgresFileRepository.h"
#include "backend/application/storage/postgres/PostgresGenerationAuditRepository/PostgresGenerationAuditRepository.h"
#include "backend/application/storage/postgres/PostgresKnowledgeRepository/PostgresKnowledgeRepository.h"
#include "backend/application/storage/postgres/PostgresMessageWriteRepository/PostgresMessageWriteRepository.h"
#include "backend/application/storage/postgres/PostgresModelRepository/PostgresModelRepository.h"
#include "backend/application/storage/postgres/PostgresOutboxRepository/PostgresOutboxRepository.h"
#include "backend/application/storage/postgres/PostgresTrainingRepository/PostgresTrainingRepository.h"
#include "backend/application/storage/postgres/PostgresMessageRepository/PostgresMessageRepository.h"
#include "backend/application/storage/postgres/PostgresWorkspaceRepository/PostgresWorkspaceRepository.h"
#include "backend/application/storage/postgres/PostgresStore/PostgresStore.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace webserver::phase11
{
namespace
{

using namespace storage_detail;
thread_local std::unordered_map<const PostgresStore *, PGconn *> activeTransactions;

} // namespace

class PostgresStore::Transaction final : public ITransaction
{
public:
    Transaction(PostgresStore &store, PostgresConnectionPool::Lease lease)
        : store_(store), lease_(std::move(lease)), owner_(std::this_thread::get_id())
    {
        const auto [_, inserted] = activeTransactions.emplace(&store_, lease_.get());
        if (!inserted)
            throw std::logic_error("nested PostgreSQL transactions are not supported");
        try
        {
            command(lease_.get(), "BEGIN");
        }
        catch (...)
        {
            activeTransactions.erase(&store_);
            throw;
        }
    }

    ~Transaction() override
    {
        if (!finished_)
        {
            try { rollback(); }
            catch (...) { activeTransactions.erase(&store_); }
        }
    }

    void commit() override { finish("COMMIT"); }
    void rollback() override { finish("ROLLBACK"); }

private:
    void finish(std::string_view action)
    {
        if (finished_)
            throw std::logic_error("transaction already completed");
        if (owner_ != std::this_thread::get_id())
            throw std::logic_error("transaction completed on a different thread");
        try
        {
            command(lease_.get(), action);
        }
        catch (...)
        {
            activeTransactions.erase(&store_);
            finished_ = true;
            lease_.reset();
            throw;
        }
        activeTransactions.erase(&store_);
        finished_ = true;
        lease_.reset();
        if(action=="COMMIT"&&store_.outboxWriter_)store_.outboxWriter_->committed();
    }

    PostgresStore &store_;
    PostgresConnectionPool::Lease lease_;
    std::thread::id owner_;
    bool finished_ = false;
};

PostgresStore::PostgresStore(PostgresPoolConfig config)
    : pool_(std::move(config))
{
}

PostgresStore::ConnectionScope PostgresStore::connection() const
{
    if (const auto found = activeTransactions.find(this);
        found != activeTransactions.end())
        return {{}, found->second};
    ConnectionScope scope;
    scope.lease.emplace(pool_.acquire());
    scope.connection = scope.lease->get();
    return scope;
}

std::unique_ptr<ITransaction> PostgresStore::beginTransaction()
{
    return std::make_unique<Transaction>(*this, pool_.acquire());
}

std::vector<webserver::chat::UserCard>PostgresStore::searchUsers(UserId user,std::string_view query,UserId after,std::size_t limit)const{auto scope=connection();return PostgresWorkspaceRepository::users(scope.connection,std::move(user),std::move(query),std::move(after),std::move(limit));}
std::vector<Message>PostgresStore::searchVisibleMessages(UserId user,ConversationId conversation,std::string_view query,MessageId before,std::size_t limit)const{auto scope=connection();return PostgresWorkspaceRepository::search(scope.connection,std::move(user),std::move(conversation),std::move(query),std::move(before),std::move(limit));}
webserver::chat::ConversationPreference PostgresStore::preference(UserId user,ConversationId conversation)const{auto scope=connection();return PostgresWorkspaceRepository::preference(scope.connection,std::move(user),std::move(conversation));}
webserver::chat::PreferenceWrite PostgresStore::writePreference(UserId user,ConversationId conversation,bool muted){auto scope=connection();return PostgresWorkspaceRepository::writePreference(scope.connection,std::move(user),std::move(conversation),std::move(muted));}

void PostgresStore::requireMigrations(std::size_t through) const
{
    auto scope=connection();
    const auto result=query(scope.connection,
        "SELECT count(*) FROM phase11_schema_migrations WHERE version BETWEEN 1 AND $1", {number(through)});
    if(std::stoull(result.value(0,0))!=through)
        throw std::runtime_error("Business requires database migrations through 024; no automatic migrations performed");
}

bool PostgresStore::healthy() const noexcept
{
    try
    {
        auto scope = connection();
        return query(scope.connection, "SELECT 1").rows() == 1;
    }
    catch (...)
    {
        return false;
    }
}

User PostgresStore::create(User user)
{
    auto scope=connection();
    return PostgresAccountRepository::create(scope.connection, std::move(user));
}

std::optional<User> PostgresStore::findUser(UserId id) const
{
    auto scope=connection();
    return PostgresAccountRepository::findUser(scope.connection, std::move(id));
}

std::optional<User> PostgresStore::findUserByName(std::string_view username) const
{
    auto scope=connection();
    return PostgresAccountRepository::findUserByName(scope.connection, std::move(username));
}

bool PostgresStore::updateProfile(UserId id, std::string displayName,
                                  std::string biography)
{
    auto scope=connection();
    return PostgresAccountRepository::updateProfile(scope.connection, std::move(id), std::move(displayName), std::move(biography));
}

// INSERT的唯一约束在并发确认时等待对方事务；收据成功与资料更新一起提交。
ProfileWriteResult PostgresStore::writeProfileOnce(UserId user,std::string operation,std::string displayName,std::string biography)
{
    auto transaction=activeTransactions.contains(this)?nullptr:beginTransaction();
    auto scope=connection();
    auto result=PostgresAccountRepository::writeProfileOnce(scope.connection, std::move(user), std::move(operation), std::move(displayName), std::move(biography));
    if(transaction)transaction->commit();
    return result;
}
bool PostgresStore::updateAvatar(UserId id, std::optional<FileId> avatar)
{
    auto scope=connection();
    return PostgresAccountRepository::updateAvatar(scope.connection, std::move(id), std::move(avatar));
}

Session PostgresStore::createSession(Session session)
{
    auto scope=connection();
    return PostgresAccountRepository::createSession(scope.connection, std::move(session));
}

std::optional<Session> PostgresStore::findSessionByTokenHash(
    std::string_view tokenHash) const
{
    auto scope=connection();
    return PostgresAccountRepository::findSessionByTokenHash(scope.connection, std::move(tokenHash));
}

bool PostgresStore::revokeSession(std::string_view sessionId, TimePoint now)
{
    auto scope=connection();
    return PostgresAccountRepository::revokeSession(scope.connection, std::move(sessionId), std::move(now));
}

FriendRequest PostgresStore::createFriendRequest(FriendRequest request)
{
    auto scope=connection();
    return PostgresContactsRepository::createFriendRequest(scope.connection, std::move(request));
}

std::optional<FriendRequest> PostgresStore::findFriendRequest(
    FriendRequestId id) const
{
    auto scope=connection();
    return PostgresContactsRepository::findFriendRequest(scope.connection, std::move(id));
}

bool PostgresStore::decideFriendRequest(FriendRequestId id, UserId receiver,
                                        FriendRequestState decision, TimePoint now)
{
    auto scope=connection();
    return PostgresContactsRepository::decideFriendRequest(scope.connection, std::move(id), std::move(receiver), std::move(decision), std::move(now));
}

bool PostgresStore::areFriends(UserId first, UserId second) const
{
    auto scope=connection();
    return PostgresContactsRepository::areFriends(scope.connection, std::move(first), std::move(second));
}

std::vector<UserId> PostgresStore::listFriendIds(UserId user) const
{
    auto scope=connection();
    return PostgresContactsRepository::listFriendIds(scope.connection, std::move(user));
}

std::vector<FriendRequest> PostgresStore::pendingRequests(UserId receiver) const
{
    auto scope=connection();
    return PostgresContactsRepository::pendingRequests(scope.connection, std::move(receiver));
}

bool PostgresStore::removeFriendship(UserId first, UserId second)
{
    auto scope=connection();
    return PostgresContactsRepository::removeFriendship(scope.connection, std::move(first), std::move(second));
}

ContactSettings PostgresStore::contactSettings(UserId owner,
                                                UserId contact) const
{
    auto scope=connection();
    return PostgresContactsRepository::contactSettings(scope.connection, std::move(owner), std::move(contact));
}

bool PostgresStore::updateContactSettings(UserId owner, UserId contact,
                                          std::string remark,
                                          bool notificationsMuted)
{
    auto scope=connection();
    return PostgresContactsRepository::updateContactSettings(scope.connection, std::move(owner), std::move(contact), std::move(remark), std::move(notificationsMuted));
}

bool PostgresStore::blockUser(UserId blocker, UserId blocked, TimePoint now)
{
    auto scope=connection();
    return PostgresContactsRepository::blockUser(scope.connection, std::move(blocker), std::move(blocked), std::move(now));
}

bool PostgresStore::unblockUser(UserId blocker, UserId blocked)
{
    auto scope=connection();
    return PostgresContactsRepository::unblockUser(scope.connection, std::move(blocker), std::move(blocked));
}

bool PostgresStore::isBlocked(UserId blocker, UserId blocked) const
{
    auto scope=connection();
    return PostgresContactsRepository::isBlocked(scope.connection, std::move(blocker), std::move(blocked));
}

bool PostgresStore::blockedEitherDirection(UserId first, UserId second) const
{
    auto scope=connection();
    return PostgresContactsRepository::blockedEitherDirection(scope.connection, std::move(first), std::move(second));
}

std::vector<UserId> PostgresStore::listBlockedIds(UserId blocker) const
{
    auto scope=connection();
    return PostgresContactsRepository::listBlockedIds(scope.connection, std::move(blocker));
}

bool PostgresStore::updateLastActive(UserId user, TimePoint now)
{
    auto scope=connection();
    return PostgresContactsRepository::updateLastActive(scope.connection, std::move(user), std::move(now));
}

std::optional<TimePoint> PostgresStore::lastActive(UserId user) const
{
    auto scope=connection();
    return PostgresContactsRepository::lastActive(scope.connection, std::move(user));
}

Conversation PostgresStore::createConversation(
    Conversation conversation, std::vector<ConversationMember> members)
{
    auto scope=connection();
    return PostgresConversationRepository::createConversation(scope.connection, scope.lease.has_value(), std::move(conversation), std::move(members));
}

std::optional<Conversation> PostgresStore::findConversation(ConversationId id) const
{
    auto scope=connection();
    return PostgresConversationRepository::findConversation(scope.connection, activeTransactions.contains(this), std::move(id));
}

std::optional<ConversationMember> PostgresStore::findMember(
    ConversationId conversation, UserId user) const
{
    auto scope=connection();
    return PostgresConversationRepository::findMember(scope.connection, std::move(conversation), std::move(user));
}

std::optional<Conversation> PostgresStore::findDirectConversation(
    UserId first, UserId second) const
{
    auto scope=connection();
    return PostgresConversationRepository::findDirectConversation(scope.connection, std::move(first), std::move(second));
}

std::vector<Conversation> PostgresStore::listConversations(UserId user) const
{
    auto scope=connection();
    return PostgresConversationRepository::listConversations(scope.connection, std::move(user));
}

std::vector<ConversationSummary> PostgresStore::listConversationSummaries(
    UserId user) const
{
    auto scope=connection();
    return PostgresConversationRepository::listConversationSummaries(scope.connection, std::move(user));
}

std::vector<ConversationMember> PostgresStore::listMembers(
    ConversationId conversation) const
{
    auto scope=connection();
    return PostgresConversationRepository::listMembers(scope.connection, std::move(conversation));
}

bool PostgresStore::addMember(ConversationMember member)
{
    auto scope=connection();
    return PostgresConversationRepository::addMember(scope.connection, std::move(member));
}

bool PostgresStore::updateConversationProfile(
    ConversationId conversation, std::string title,
    std::string announcement)
{
    auto scope=connection();
    return PostgresConversationRepository::updateConversationProfile(scope.connection, std::move(conversation), std::move(title), std::move(announcement));
}

bool PostgresStore::dissolveConversation(ConversationId conversation,
                                         UserId actor, TimePoint now)
{
    auto scope=connection();
    return PostgresConversationRepository::dissolveConversation(scope.connection, std::move(conversation), std::move(actor), std::move(now));
}

bool PostgresStore::updateMemberRole(ConversationId conversation, UserId user,
                                     MemberRole role)
{
    auto scope=connection();
    return PostgresConversationRepository::updateMemberRole(scope.connection, std::move(conversation), std::move(user), std::move(role));
}

bool PostgresStore::updateMemberMute(
    ConversationId conversation, UserId user,
    std::optional<TimePoint> mutedUntil)
{
    auto scope=connection();
    return PostgresConversationRepository::updateMemberMute(scope.connection, std::move(conversation), std::move(user), std::move(mutedUntil));
}

bool PostgresStore::removeMember(ConversationId conversation, UserId user)
{
    auto scope=connection();
    return PostgresConversationRepository::removeMember(scope.connection, std::move(conversation), std::move(user));
}

bool PostgresStore::updateDelivered(ConversationId conversation, UserId user,
                                    std::uint64_t sequence)
{
    auto scope=connection();
    return PostgresConversationRepository::updateDelivered(scope.connection, std::move(conversation), std::move(user), std::move(sequence));
}

bool PostgresStore::updateRead(ConversationId conversation, UserId user,
                               std::uint64_t sequence)
{
    auto scope=connection();
    return PostgresConversationRepository::updateRead(scope.connection, std::move(conversation), std::move(user), std::move(sequence));
}

AppendMessageResult PostgresStore::appendMessage(Message message)
{
    auto scope=connection();
    return PostgresMessageWriteRepository::appendMessage(scope.connection, scope.lease.has_value(), std::move(message));
}

std::vector<Message> PostgresStore::messagesAfter(
    ConversationId conversation, std::uint64_t sequence,
    std::size_t limit) const
{
    auto scope=connection();
    return PostgresMessageRepository::after(scope.connection,std::move(conversation),std::move(sequence),std::move(limit));
}

std::vector<Message> PostgresStore::messagesBefore(
    ConversationId conversation, std::uint64_t before, std::size_t limit) const
{
    auto scope=connection();
    return PostgresMessageRepository::before(scope.connection,std::move(conversation),std::move(before),std::move(limit));
}

std::optional<Message> PostgresStore::findOwnMessage(
    ConversationId conversation, UserId sender, std::string_view clientMessageId) const
{
    auto scope=connection();
    return PostgresMessageRepository::own(scope.connection,std::move(conversation),std::move(sender),std::move(clientMessageId));
}

std::vector<Message> PostgresStore::searchMessages(
    ConversationId conversation, std::string_view text,
    std::size_t limit) const
{
    auto scope=connection();
    return PostgresMessageRepository::search(scope.connection,std::move(conversation),std::move(text),std::move(limit));
}

std::optional<Message> PostgresStore::findMessage(MessageId id) const
{
    auto scope=connection();
    return PostgresMessageRepository::find(scope.connection,std::move(id));
}

RecallMessageResult PostgresStore::recallMessage(MessageId id, UserId actor,
                                                 TimePoint now)
{
    auto scope=connection();
    return PostgresMessageWriteRepository::recallMessage(scope.connection, std::move(id), std::move(actor), std::move(now));
}

FileObject PostgresStore::createFile(FileObject file)
{
    auto scope=connection();
    return PostgresFileRepository::createFile(scope.connection, std::move(file));
}

std::optional<FileObject> PostgresStore::findFile(FileId id) const
{
    auto scope=connection();
    return PostgresFileRepository::findFile(scope.connection, std::move(id));
}

bool PostgresStore::updateFileProgress(FileId id, UserId owner,
                                       std::uint64_t expectedOffset,
                                       std::uint64_t uploadedSize)
{
    auto scope=connection();
    return PostgresFileRepository::updateFileProgress(scope.connection, std::move(id), std::move(owner), std::move(expectedOffset), std::move(uploadedSize));
}

bool PostgresStore::completeFile(FileId id, UserId owner,
                                 std::string sha256, TimePoint now)
{
    auto scope=connection();
    return PostgresFileRepository::completeFile(scope.connection, std::move(id), std::move(owner), std::move(sha256), std::move(now));
}

bool PostgresStore::failFile(FileId id, UserId owner)
{
    auto scope=connection();
    return PostgresFileRepository::failFile(scope.connection, std::move(id), std::move(owner));
}

bool PostgresStore::attachFile(MessageId message, FileId file)
{
    auto scope=connection();
    return PostgresFileRepository::attachFile(scope.connection, std::move(message), std::move(file));
}

std::vector<MessageAttachment> PostgresStore::attachmentsForMessage(
    MessageId message) const
{
    auto scope=connection();
    return PostgresFileRepository::attachmentsForMessage(scope.connection, std::move(message));
}

bool PostgresStore::canAccessFile(UserId user, FileId file) const
{
    auto scope=connection();
    return PostgresFileRepository::canAccessFile(scope.connection, std::move(user), std::move(file));
}

OutboxEvent PostgresStore::appendOutbox(OutboxEvent event)
{
    auto scope=connection();
    return PostgresOutboxRepository::appendOutbox(scope.connection, scope.lease.has_value(), outboxWriter_, std::move(event));
}

std::vector<OutboxEvent> PostgresStore::pendingOutbox(std::size_t limit) const
{
    auto scope=connection();
    return PostgresOutboxRepository::pendingOutbox(scope.connection, std::move(limit));
}

bool PostgresStore::markPublished(OutboxEventId id, TimePoint now)
{
    auto scope=connection();
    return PostgresOutboxRepository::markPublished(scope.connection, std::move(id), std::move(now));
}

ModelNode PostgresStore::createModelNode(ModelNode node)
{
    auto scope=connection();
    return PostgresModelRepository::createModelNode(scope.connection, std::move(node));
}

ModelVersion PostgresStore::createModelVersion(ModelVersion version)
{
    auto scope=connection();
    return PostgresModelRepository::createModelVersion(scope.connection, std::move(version));
}

std::pair<ModelVersion, ModelTrainingLineage>
PostgresStore::createTrainedModelVersion(ModelVersion version,
                                         ModelTrainingLineage lineage)
{
    auto scope=connection();
    return PostgresModelRepository::createTrainedModelVersion(scope.connection, scope.lease.has_value(), std::move(version), std::move(lineage));
}

ModelEdge PostgresStore::createModelEdge(ModelEdge edge)
{
    auto scope=connection();
    return PostgresModelRepository::createModelEdge(scope.connection, std::move(edge));
}

bool PostgresStore::bindAiAccount(ModelBinding binding)
{
    auto scope=connection();
    return PostgresModelRepository::bindAiAccount(scope.connection, std::move(binding));
}

std::optional<ModelNode> PostgresStore::findModelNode(ModelNodeId id) const
{
    auto scope=connection();
    return PostgresModelRepository::findModelNode(scope.connection, std::move(id));
}

std::vector<ModelNode> PostgresStore::modelNodes(ModelNodeId after,std::size_t limit)const
{
    auto scope=connection();
    return PostgresModelRepository::modelNodes(scope.connection, std::move(after), std::move(limit));
}
std::vector<ModelVersion> PostgresStore::modelVersions(ModelNodeId node,ModelVersionId after,std::size_t limit)const
{
    auto scope=connection();
    return PostgresModelRepository::modelVersions(scope.connection, std::move(node), std::move(after), std::move(limit));
}

std::optional<ModelVersion> PostgresStore::findModelVersion(ModelVersionId id) const
{
    auto scope=connection();
    return PostgresModelRepository::findModelVersion(scope.connection, std::move(id));
}

std::optional<ModelTrainingLineage>
PostgresStore::findModelTrainingLineage(ModelVersionId version) const
{
    auto scope=connection();
    return PostgresModelRepository::findModelTrainingLineage(scope.connection, std::move(version));
}

std::optional<ModelBinding> PostgresStore::findModelBinding(UserId aiUser) const
{
    auto scope=connection();
    return PostgresModelRepository::findModelBinding(scope.connection, std::move(aiUser));
}

std::vector<ModelEdge> PostgresStore::modelEdges() const
{
    auto scope=connection();
    return PostgresModelRepository::modelEdges(scope.connection);
}

bool PostgresStore::activateModelVersion(ModelNodeId node, ModelVersionId version,
                                         ModelActivationAudit audit)
{
    auto scope=connection();
    return PostgresModelRepository::activateModelVersion(scope.connection, std::move(node), std::move(version), std::move(audit));
}

ModelActivationAudit PostgresStore::appendModelActivationAudit(
    ModelActivationAudit audit)
{
    auto scope=connection();
    return PostgresModelRepository::appendModelActivationAudit(scope.connection, std::move(audit));
}

std::vector<ModelActivationAudit> PostgresStore::modelActivationAudits(
    ModelNodeId node, std::size_t limit) const
{
    auto scope=connection();
    return PostgresModelRepository::modelActivationAudits(scope.connection, std::move(node), std::move(limit));
}

bool PostgresStore::upsertGenerationAudit(AiGenerationAudit audit)
{
    auto scope=connection();
    return PostgresGenerationAuditRepository::upsertGenerationAudit(scope.connection, std::move(audit));
}

std::optional<AiGenerationAudit> PostgresStore::findGenerationAudit(
    std::string_view generationId) const
{
    auto scope=connection();
    return PostgresGenerationAuditRepository::findGenerationAudit(scope.connection, std::move(generationId));
}

ModelEvaluationSuite PostgresStore::createEvaluationSuite(
    ModelEvaluationSuite suite, std::vector<ModelEvaluationCase> cases)
{
    auto scope=connection();
    return PostgresEvaluationRepository::createEvaluationSuite(scope.connection, scope.lease.has_value(), std::move(suite), std::move(cases));
}

std::optional<ModelEvaluationSuite> PostgresStore::findEvaluationSuite(
    ModelEvaluationSuiteId id) const
{
    auto scope=connection();
    return PostgresEvaluationRepository::findEvaluationSuite(scope.connection, std::move(id));
}

std::vector<ModelEvaluationCase> PostgresStore::evaluationCases(
    ModelEvaluationSuiteId suite) const
{
    auto scope=connection();
    return PostgresEvaluationRepository::evaluationCases(scope.connection, std::move(suite));
}

ModelEvaluationRun PostgresStore::createEvaluationRun(ModelEvaluationRun run)
{
    auto scope=connection();
    return PostgresEvaluationRepository::createEvaluationRun(scope.connection, std::move(run));
}

std::optional<ModelEvaluationRun> PostgresStore::findEvaluationRun(
    ModelEvaluationRunId id) const
{
    auto scope=connection();
    return PostgresEvaluationRepository::findEvaluationRun(scope.connection, std::move(id));
}

bool PostgresStore::appendEvaluationCaseResult(
    ModelEvaluationCaseResult result)
{
    auto scope=connection();
    return PostgresEvaluationRepository::appendEvaluationCaseResult(scope.connection, std::move(result));
}

std::vector<ModelEvaluationCaseResult>
PostgresStore::evaluationCaseResults(ModelEvaluationRunId run) const
{
    auto scope=connection();
    return PostgresEvaluationRepository::evaluationCaseResults(scope.connection, std::move(run));
}

bool PostgresStore::finishEvaluationRun(ModelEvaluationRun run)
{
    auto scope=connection();
    return PostgresEvaluationRepository::finishEvaluationRun(scope.connection, std::move(run));
}

bool PostgresStore::upsertActivationPolicy(ModelActivationPolicy policy)
{
    auto scope=connection();
    return PostgresEvaluationRepository::upsertActivationPolicy(scope.connection, std::move(policy));
}

std::optional<ModelActivationPolicy> PostgresStore::findActivationPolicy(
    ModelNodeId node) const
{
    auto scope=connection();
    return PostgresEvaluationRepository::findActivationPolicy(scope.connection, std::move(node));
}

std::optional<ModelEvaluationRun>
PostgresStore::latestCompletedEvaluationRun(
    ModelVersionId version, ModelEvaluationSuiteId suite) const
{
    auto scope=connection();
    return PostgresEvaluationRepository::latestCompletedEvaluationRun(scope.connection, std::move(version), std::move(suite));
}

KnowledgeDocument PostgresStore::createKnowledgeDocument(
    KnowledgeDocument document, std::vector<KnowledgeChunk> chunks)
{
    auto scope=connection();
    return PostgresKnowledgeRepository::createKnowledgeDocument(scope.connection, scope.lease.has_value(), std::move(document), std::move(chunks));
}

bool PostgresStore::updateKnowledgeDocumentState(
    KnowledgeDocumentId document, KnowledgeDocumentState expected,
    KnowledgeDocumentState next, std::string failureDetail,
    std::optional<TimePoint> indexedAt)
{
    auto scope=connection();
    return PostgresKnowledgeRepository::updateKnowledgeDocumentState(scope.connection, std::move(document), std::move(expected), std::move(next), std::move(failureDetail), std::move(indexedAt));
}

std::optional<KnowledgeDocument> PostgresStore::findKnowledgeDocument(
    KnowledgeDocumentId id) const
{
    auto scope=connection();
    return PostgresKnowledgeRepository::findKnowledgeDocument(scope.connection, std::move(id));
}

std::optional<KnowledgeChunk> PostgresStore::findKnowledgeChunk(
    KnowledgeChunkId id) const
{
    auto scope=connection();
    return PostgresKnowledgeRepository::findKnowledgeChunk(scope.connection, std::move(id));
}

std::vector<KnowledgeChunk> PostgresStore::knowledgeChunks(
    KnowledgeDocumentId document) const
{
    auto scope=connection();
    return PostgresKnowledgeRepository::knowledgeChunks(scope.connection, std::move(document));
}

std::vector<KnowledgeDocument> PostgresStore::privateKnowledgeDocuments(
    UserId owner) const
{
    auto scope=connection();
    return PostgresKnowledgeRepository::privateKnowledgeDocuments(scope.connection, std::move(owner));
}

std::vector<KnowledgeDocument>
PostgresStore::conversationKnowledgeDocuments(
    ConversationId conversation) const
{
    auto scope=connection();
    return PostgresKnowledgeRepository::conversationKnowledgeDocuments(scope.connection, std::move(conversation));
}

std::vector<KnowledgeChunk> PostgresStore::allReadyKnowledgeChunks() const
{
    auto scope=connection();
    return PostgresKnowledgeRepository::allReadyKnowledgeChunks(scope.connection);
}

bool PostgresStore::saveRagRetrieval(
    RagRetrieval retrieval, std::vector<RagCitation> citations)
{
    auto scope=connection();
    return PostgresKnowledgeRepository::saveRagRetrieval(scope.connection, scope.lease.has_value(), std::move(retrieval), std::move(citations));
}

std::optional<RagRetrieval> PostgresStore::findRagRetrieval(
    std::string_view generationId) const
{
    auto scope=connection();
    return PostgresKnowledgeRepository::findRagRetrieval(scope.connection, std::move(generationId));
}

std::vector<RagCitation> PostgresStore::ragCitations(
    std::string_view generationId) const
{
    auto scope=connection();
    return PostgresKnowledgeRepository::ragCitations(scope.connection, std::move(generationId));
}

TrainingCandidate PostgresStore::createTrainingCandidate(
    TrainingCandidate candidate)
{
    auto scope=connection();
    return PostgresTrainingRepository::createTrainingCandidate(scope.connection, std::move(candidate));
}

std::optional<TrainingCandidate> PostgresStore::findTrainingCandidate(
    TrainingCandidateId id) const
{
    auto scope=connection();
    return PostgresTrainingRepository::findTrainingCandidate(scope.connection, std::move(id));
}

std::optional<TrainingCandidate> PostgresStore::findActiveTrainingCandidate(
    UserId owner, MessageId sourceMessage) const
{
    auto scope=connection();
    return PostgresTrainingRepository::findActiveTrainingCandidate(scope.connection, std::move(owner), std::move(sourceMessage));
}

std::vector<TrainingCandidate> PostgresStore::trainingCandidatesForOwner(
    UserId owner) const
{
    auto scope=connection();
    return PostgresTrainingRepository::trainingCandidatesForOwner(scope.connection, std::move(owner));
}

std::vector<TrainingCandidate> PostgresStore::trainingCandidatesByState(
    TrainingCandidateState state) const
{
    auto scope=connection();
    return PostgresTrainingRepository::trainingCandidatesByState(scope.connection, std::move(state));
}

bool PostgresStore::updateTrainingCandidate(TrainingCandidate candidate)
{
    auto scope=connection();
    return PostgresTrainingRepository::updateTrainingCandidate(scope.connection, std::move(candidate));
}

bool PostgresStore::transitionTrainingCandidate(
    TrainingCandidateId id, TrainingCandidateState expected,
    TrainingCandidateState next, std::optional<DatasetVersionId> dataset)
{
    auto scope=connection();
    return PostgresTrainingRepository::transitionTrainingCandidate(scope.connection, std::move(id), std::move(expected), std::move(next), std::move(dataset));
}

DatasetVersion PostgresStore::createDatasetVersion(DatasetVersion version)
{
    auto scope=connection();
    return PostgresTrainingRepository::createDatasetVersion(scope.connection, scope.lease.has_value(), std::move(version));
}


std::optional<DatasetVersion> PostgresStore::findDatasetVersion(
    DatasetVersionId id) const
{
    auto scope=connection();
    return PostgresTrainingRepository::findDatasetVersion(scope.connection, std::move(id));
}

} // namespace webserver::phase11

namespace webserver::phase11 {
bool PostgresStore::markPublishedBatch(const std::vector<OutboxEventId> &ids,TimePoint now) {
    auto scope=connection();
    return PostgresOutboxRepository::markPublishedBatch(scope.connection,ids,now);
}
}
