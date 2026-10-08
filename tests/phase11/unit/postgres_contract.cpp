#include "server/phase11/storage/postgres/PostgresStore/PostgresStore.h"
#include "server/phase11/ai/ModelEvaluationService/ModelEvaluationService.h"
#include "server/phase11/ai/DeterministicKnowledgeIndex/DeterministicKnowledgeIndex.h"
#include "server/phase11/ai/RagService/RagService.h"
#include "tests/phase11/unit/RepositoryContract.h"

#if defined(WEBSERVER_HAS_SODIUM)
#include "server/phase11/business/AuthService/AuthService.h"
#include "server/phase11/security/InMemoryLoginRateLimiter/InMemoryLoginRateLimiter.h"
#include "server/phase11/security/SodiumCredentialCodec/SodiumCredentialCodec.h"
#endif

#include <libpq-fe.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace webserver::phase11;

namespace
{

/**
 * 契约测试必须从空数据开始。这里只清理专用 phase11_test 数据库；连接串由环境变量
 * 注入，源码和测试日志都不保存口令。
 */
void clearTestDatabase(const std::string &connectionString)
{
    PGconn *connection = PQconnectdb(connectionString.c_str());
    if (!connection || PQstatus(connection) != CONNECTION_OK)
    {
        const std::string error = connection ? PQerrorMessage(connection)
                                             : "libpq allocation failed";
        if (connection) PQfinish(connection);
        throw std::runtime_error("test database connection failed: " + error);
    }

    PGresult *result = PQexec(
        connection,
        "TRUNCATE TABLE users, model_nodes, dataset_versions, "
        "model_evaluation_suites, knowledge_documents, rag_retrievals, "
        "outbox_events "
        "RESTART IDENTITY CASCADE");
    const bool succeeded = result && PQresultStatus(result) == PGRES_COMMAND_OK;
    const std::string error = succeeded ? std::string{} : PQerrorMessage(connection);
    if (result) PQclear(result);
    PQfinish(connection);
    if (!succeeded)
        throw std::runtime_error("test database cleanup failed: " + error);
}

class CleanupGuard final
{
public:
    explicit CleanupGuard(std::string connectionString)
        : connectionString_(std::move(connectionString))
    {
    }

    ~CleanupGuard()
    {
        try { clearTestDatabase(connectionString_); }
        catch (const std::exception &error)
        {
            std::cerr << "post-test cleanup warning: " << error.what() << '\n';
        }
    }

private:
    std::string connectionString_;
};

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(std::string{"extended PostgreSQL contract: "} +
                                 message);
}

/**
 * RepositoryContract 验证聊天主链路；这里覆盖其余 PostgreSQL 映射。可以把它理解成
 * 仓库验收时逐个开抽屉：不仅大门能开，模型、训练、游标等抽屉也都要取放正确。
 */
void runExtendedContract(PostgresStore &store, TimePoint now)
{
    const auto alice = store.findUserByName("contract-alice");
    const auto bob = store.findUserByName("contract-bob");
    require(alice && bob, "contract users could not be reloaded");
    require(store.updateProfile(alice->id, "Alice Updated", "postgres"),
            "profile update failed");
    const auto updatedAlice = store.findUser(alice->id);
    require(updatedAlice && updatedAlice->displayName == "Alice Updated" &&
                updatedAlice->biography == "postgres",
            "profile fields were not persisted");

    require(store.revokeSession("contract-session", now),
            "session revocation failed");
    const auto revoked = store.findSessionByTokenHash("contract-token-hash");
    require(revoked && revoked->revokedAt.has_value(),
            "revoked session did not retain its timestamp");

    const auto direct = store.findDirectConversation(alice->id, bob->id);
    require(direct.has_value(), "direct conversation lookup failed");
    require(store.listConversations(alice->id).size() == 1,
            "conversation membership listing failed");
    require(store.updateDelivered(direct->id, bob->id, 1),
            "delivery cursor update failed");
    require(store.updateRead(direct->id, bob->id, 1),
            "read cursor update failed");
    require(!store.updateDelivered(direct->id, bob->id, 1) &&
                !store.updateRead(direct->id, bob->id, 1),
            "cursor update was not idempotent");
    const auto member = store.findMember(direct->id, bob->id);
    require(member && member->lastDeliveredSequence == 1 &&
                member->lastReadSequence == 1,
            "member cursors were not persisted");
    const auto messages = store.messagesAfter(direct->id, 0, 10);
    require(messages.size() == 1 &&
                store.findMessage(messages.front().id).has_value() &&
                store.searchMessages(direct->id, "ell", 10).size() == 1,
            "message lookup or search failed");

    // 文件元数据与正文目录是两套设施：Repository 契约只验证登记卡、附件关系和
    // 权限查询；字节的分片写入与哈希封存由独立 LocalFileStorage 测试覆盖。
    auto file = store.createFile(
        {0, alice->id, "contract.png", "image/png", 8, 0, {},
         FileState::Pending, now, {}});
    require(file.id != 0 && store.findFile(file.id).has_value(),
            "file metadata insert failed");
    require(store.updateFileProgress(file.id, alice->id, 0, 8) &&
                !store.updateFileProgress(file.id, alice->id, 0, 8),
            "file upload cursor compare-and-set failed");
    const std::string digest(64, 'a');
    require(store.completeFile(file.id, alice->id, digest, now),
            "file completion failed");
    file = *store.findFile(file.id);
    require(file.state == FileState::Ready && file.sha256 == digest &&
                file.readyAt.has_value(),
            "ready file metadata was not persisted");
    require(store.attachFile(messages.front().id, file.id) &&
                !store.attachFile(messages.front().id, file.id),
            "message attachment uniqueness failed");
    const auto hydratedMessage = store.findMessage(messages.front().id);
    require(hydratedMessage && hydratedMessage->attachments.size() == 1 &&
                hydratedMessage->attachments.front().fileId == file.id,
            "message attachment hydration failed");
    require(store.canAccessFile(alice->id, file.id) &&
                store.canAccessFile(bob->id, file.id),
            "owner or direct member could not access attachment");

    const auto recalled = store.recallMessage(messages.front().id, alice->id, now);
    const auto recalledAgain =
        store.recallMessage(messages.front().id, alice->id, now);
    require(recalled.changed && !recalledAgain.changed &&
                recalled.message.state == MessageState::Recalled &&
                store.searchMessages(direct->id, "ell", 10).empty(),
            "message recall persistence failed");

    User charlie{0, "contract-charlie", "Charlie", {}, "hash-c",
                 false, false, now};
    charlie = store.create(std::move(charlie));
    require(!store.canAccessFile(charlie.id, file.id),
            "non-member accessed a private attachment");
    require(store.updateAvatar(alice->id, file.id),
            "avatar reference update failed");
    const auto avatarOwner = store.findUser(alice->id);
    require(avatarOwner && avatarOwner->avatarFileId == file.id &&
                store.canAccessFile(charlie.id, file.id),
            "authenticated avatar visibility was not persisted");
    const auto pending = store.createFriendRequest(
        {0, bob->id, charlie.id, FriendRequestState::Pending, now, {}});
    require(store.findFriendRequest(pending.id).has_value() &&
                store.pendingRequests(charlie.id).size() == 1,
            "pending friend request lookup failed");

    auto group = store.createConversation(
        {0, ConversationKind::Group, "PostgreSQL Study", alice->id, 1, now},
        {{0, alice->id, MemberRole::Owner, 0, 0, now}});
    require(store.addMember(
                {group.id, charlie.id, MemberRole::Member, 0, 0, now}),
            "group member insert failed");
    require(!store.addMember(
                {group.id, charlie.id, MemberRole::Member, 0, 0, now}),
            "duplicate group member was inserted");

    DeterministicKnowledgeIndex knowledgeIndex;
    RagService rag(store, store, store, knowledgeIndex);
    const auto privateKnowledge = rag.registerDocument(
        alice->id, {}, "PostgreSQL 私人知识", "private.md", "text/markdown",
        "连接池负责限制数据库连接并发。", now);
    const auto sharedKnowledge = rag.registerDocument(
        alice->id, group.id, "PostgreSQL 群知识", "group.md", "text/markdown",
        "RAG 引用链把生成请求和知识片段持久关联。", now);
    const auto ragContext = rag.retrieveForGeneration(
        "postgres-rag-generation", charlie.id, group.id,
        "生成请求 知识片段 引用链", 6, now);
    require(privateKnowledge.state == KnowledgeDocumentState::Ready &&
                sharedKnowledge.state == KnowledgeDocumentState::Ready &&
                !ragContext.citations.empty() &&
                ragContext.citations.front().documentId == sharedKnowledge.id,
            "RAG document authorization or retrieval persistence failed");
    require(store.allReadyKnowledgeChunks().size() >= 2 &&
                store.findRagRetrieval("postgres-rag-generation") &&
                store.ragCitations("postgres-rag-generation").size() ==
                    ragContext.citations.size(),
            "RAG chunks or citation chain could not be reloaded");

    User ai{0, "contract-ai", "Contract AI", {}, "disabled",
            false, true, now};
    ai = store.create(std::move(ai));
    const auto root = store.createModelNode(
        {0, "contract-root", ModelNodeKind::Root, {}, now});
    const auto specialist = store.createModelNode(
        {0, "contract-specialist", ModelNodeKind::Specialist, {}, now});
    const auto edge = store.createModelEdge(
        {0, root.id, specialist.id, ModelEdgeKind::ParentControlsChild});
    require(edge.id != 0 && store.modelEdges().size() == 1,
            "model edge persistence failed");
    const auto specialistVersion = store.createModelVersion(
        {0, specialist.id, "onnx-inprocess", "model.onnx",
         "adapter.onnx_adapter", "sha256:specialist",
         ModelVersionState::Candidate, now});
    const auto rootVersion = store.createModelVersion(
        {0, root.id, "ollama", "root-model", {}, "sha256:root",
         ModelVersionState::Candidate, now});

    ModelEvaluationService evaluations(store, store);
    const auto evaluationSuite = evaluations.createSuite(
        "postgres-quality-gate", "v1", std::string(64, 'f'),
        "PostgreSQL evaluation contract",
        {{0, 0, "required", "prompt-a", "answer-a", 2.0, true},
         {0, 0, "optional", "prompt-b", "answer-b", 1.0, false}},
        now);
    const auto evaluationCases = store.evaluationCases(evaluationSuite.id);
    const auto persistedSuite = store.findEvaluationSuite(evaluationSuite.id);
    require(persistedSuite && persistedSuite->version == "v1" &&
                persistedSuite->checksum == std::string(64, 'f') &&
                evaluationCases.size() == 2,
            "evaluation suite cases were not persisted atomically");
    require(evaluations.configurePolicy(
                specialist.id, evaluationSuite.id, 1.0, 0.8, 3'600, now)
                .suiteId == evaluationSuite.id,
            "evaluation activation policy was not persisted");
    auto evaluationRun = evaluations.startRun(
        specialistVersion.id, evaluationSuite.id, now);
    evaluations.recordResult(evaluationRun.id, evaluationCases[0].id,
                             1.0, true, std::string(64, 'd'), 11);
    evaluations.recordResult(evaluationRun.id, evaluationCases[1].id,
                             0.7, true, std::string(64, 'e'), 13);
    evaluationRun = evaluations.completeRun(
        evaluationRun.id, now + std::chrono::seconds(2));
    require(evaluationRun.status == ModelEvaluationRunStatus::Completed &&
                evaluationRun.requiredCasesPassed &&
                store.evaluationCaseResults(evaluationRun.id).size() == 2,
            "evaluation run summary or case results were not persisted");
    const auto persistedPolicy = store.findActivationPolicy(specialist.id);
    const auto latestEvaluation = store.latestCompletedEvaluationRun(
        specialistVersion.id, evaluationSuite.id);
    const auto gateDecision = evaluations.assess(
        specialist.id, specialistVersion.id, now + std::chrono::seconds(3));
    require(persistedPolicy && latestEvaluation &&
                latestEvaluation->id == evaluationRun.id && gateDecision.allowed,
            "latest evaluation run did not satisfy persisted quality gate");
    require(store.activateModelVersion(
                specialist.id, specialistVersion.id,
                {0, specialist.id, {}, specialistVersion.id,
                 ModelActivationAction::Activate,
                 ModelActivationOutcome::Succeeded, "postgres-contract",
                 "activate contract model", now}),
            "valid model activation failed");
    require(!store.activateModelVersion(
                specialist.id, rootVersion.id,
                {0, specialist.id, specialistVersion.id, rootVersion.id,
                 ModelActivationAction::Activate,
                 ModelActivationOutcome::Succeeded, "postgres-contract",
                 "invalid cross-node activation", now}),
            "version belonging to another node was activated");
    const auto activationAudits =
        store.modelActivationAudits(specialist.id, 10);
    require(activationAudits.size() == 1 &&
                activationAudits.front().targetVersionId == specialistVersion.id &&
                activationAudits.front().outcome ==
                    ModelActivationOutcome::Succeeded,
            "successful model activation audit was not atomic");
    const auto activeNode = store.findModelNode(specialist.id);
    const auto activeVersion = store.findModelVersion(specialistVersion.id);
    require(activeNode && activeNode->activeVersionId == specialistVersion.id &&
                activeVersion && activeVersion->state == ModelVersionState::Active,
            "failed activation retired or detached the previous active version");
    require(store.bindAiAccount({ai.id, specialist.id}),
            "AI model binding failed");
    const auto binding = store.findModelBinding(ai.id);
    require(binding && binding->nodeId == specialist.id,
            "AI model binding lookup failed");
    require(!store.bindAiAccount({alice->id, specialist.id}),
            "ordinary user was accepted as an AI account");

    AiGenerationAudit generationAudit;
    generationAudit.generationId = "postgres-generation-audit";
    generationAudit.requesterId = alice->id;
    generationAudit.aiUserId = ai.id;
    generationAudit.conversationId = direct->id;
    generationAudit.promptSha256 = std::string(64, 'b');
    generationAudit.promptBytes = 12;
    generationAudit.status = AiGenerationStatus::Queued;
    generationAudit.requestedAt = now;
    require(store.upsertGenerationAudit(generationAudit),
            "queued generation audit insert failed");
    generationAudit.modelNodeId = specialist.id;
    generationAudit.modelVersionId = specialistVersion.id;
    generationAudit.runtime = "onnx-inprocess";
    generationAudit.modelArtifact = "model.onnx";
    generationAudit.adapterArtifact = "adapter.onnx_adapter";
    generationAudit.modelChecksum = "sha256:specialist";
    generationAudit.outputSha256 = std::string(64, 'c');
    generationAudit.outputBytes = 16;
    generationAudit.outputTokens = 2;
    generationAudit.status = AiGenerationStatus::Succeeded;
    generationAudit.startedAt = now;
    generationAudit.firstTokenAt = now;
    generationAudit.completedAt = now;
    generationAudit.responseMessageId = messages.front().id;
    generationAudit.revision = 4;
    require(store.upsertGenerationAudit(generationAudit),
            "terminal generation audit update failed");
    auto staleAudit = generationAudit;
    staleAudit.status = AiGenerationStatus::Running;
    staleAudit.revision = 2;
    require(!store.upsertGenerationAudit(std::move(staleAudit)),
            "stale generation audit overwrote terminal state");
    const auto storedGenerationAudit =
        store.findGenerationAudit("postgres-generation-audit");
    require(storedGenerationAudit &&
                storedGenerationAudit->status == AiGenerationStatus::Succeeded &&
                storedGenerationAudit->modelVersionId == specialistVersion.id &&
                storedGenerationAudit->outputTokens == 2 &&
                storedGenerationAudit->responseMessageId == messages.front().id,
            "generation audit fields were not persisted");

    auto candidate = store.createTrainingCandidate(
        {0, alice->id, messages.front().id, "prompt", "response",
         "consent-v1", TrainingCandidateState::Submitted, {}, now});
    const auto activeCandidate = store.findActiveTrainingCandidate(
        alice->id, messages.front().id);
    require(activeCandidate && activeCandidate->id == candidate.id,
            "active training candidate lookup failed");
    candidate.state = TrainingCandidateState::Approved;
    require(store.updateTrainingCandidate(candidate),
            "training candidate review update failed");
    const auto dataset = store.createDatasetVersion(
        {0, "contract-dataset", std::string(64, 'e'), {candidate.id}, now,
         "dataset-contract.jsonl", 1});
    candidate.state = TrainingCandidateState::Exported;
    candidate.datasetVersionId = dataset.id;
    require(store.updateTrainingCandidate(candidate),
            "training candidate export update failed");
    const auto exported = store.findTrainingCandidate(candidate.id);
    require(exported && exported->state == TrainingCandidateState::Exported &&
                exported->datasetVersionId == dataset.id,
            "training candidate export state was not persisted");
    const auto ownerCandidates = store.trainingCandidatesForOwner(alice->id);
    require(ownerCandidates.size() == 1 &&
                ownerCandidates.front().id == candidate.id,
            "training candidate owner listing failed");
    const auto persistedDataset = store.findDatasetVersion(dataset.id);
    require(persistedDataset &&
                persistedDataset->artifactPath == "dataset-contract.jsonl" &&
                persistedDataset->recordCount == 1 &&
                persistedDataset->candidates ==
                    std::vector<TrainingCandidateId>{candidate.id},
            "training dataset artifact metadata was not persisted");
}

#if defined(WEBSERVER_HAS_SODIUM)
void runSecurePostgresAuthContract(PostgresStore &store, TimePoint now)
{
    constexpr std::string_view key =
        "000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f";
    SodiumCredentialCodec credentials(key);
    InMemoryLoginRateLimiter limiter;
    AuthService auth(store, store, credentials, limiter);

    const auto user = auth.registerUser(
        "postgres-secure", "postgres-password", "PostgreSQL Secure", now);
    const auto persisted = store.findUser(user.id);
    require(persisted && persisted->passwordHash.starts_with("$argon2id$"),
            "PostgreSQL did not retain the Argon2id password hash");

    try
    {
        (void)auth.login("postgres-secure", "wrong-password", now,
                         "127.0.0.1");
        throw std::runtime_error("wrong PostgreSQL password was accepted");
    }
    catch (const ApplicationError &error)
    {
        require(error.code() == ErrorCode::Unauthenticated,
                "wrong PostgreSQL password returned an unexpected error");
    }

    const auto login = auth.login(
        "postgres-secure", "postgres-password", now, "127.0.0.1");
    require(auth.authenticate(login.sessionToken, now).has_value(),
            "PostgreSQL-backed session did not authenticate");
    require(auth.verifyCsrf(login.sessionToken, login.csrfToken, now),
            "PostgreSQL-backed CSRF verification failed");

    // 使用同一持久密钥创建新 Codec，模拟服务器进程重启。Token 指纹仍然一致，因此
    // 数据库中的旧 Session 可以继续验证；这正是密钥不能每次启动随机生成的原因。
    SodiumCredentialCodec afterRestartCredentials(key);
    InMemoryLoginRateLimiter afterRestartLimiter;
    AuthService afterRestartAuth(
        store, store, afterRestartCredentials, afterRestartLimiter);
    require(afterRestartAuth.authenticate(login.sessionToken, now).has_value(),
            "session did not survive credential codec restart");
    afterRestartAuth.logout(login.sessionToken, now);
    require(!auth.authenticate(login.sessionToken, now),
            "PostgreSQL-backed logout did not revoke the session");
}
#endif

} // namespace

int main()
{
    try
    {
        const char *raw = std::getenv("PHASE11_POSTGRES_CONNECTION");
        if (!raw || std::string(raw).empty())
            throw std::runtime_error(
                "PHASE11_POSTGRES_CONNECTION must target an isolated test database");

        const std::string connectionString = raw;
        clearTestDatabase(connectionString);
        CleanupGuard cleanup(connectionString);

        PostgresStore store({connectionString, 4, std::chrono::seconds(2)});
        if (!store.healthy())
            throw std::runtime_error("PostgresStore health check failed");

        test::runRepositoryContract(
            {store, store, store, store, store, store, store},
            std::chrono::system_clock::now());
        runExtendedContract(store, std::chrono::system_clock::now());
#if defined(WEBSERVER_HAS_SODIUM)
        runSecurePostgresAuthContract(store, std::chrono::system_clock::now());
#endif

        // 多个数据库 Worker 可以同时借用不同连接；这能发现把单个 PGconn 错误地
        // 跨线程共享、Lease 未归还或连接池唤醒丢失等问题。
        std::vector<std::future<bool>> checks;
        for (int index = 0; index < 12; ++index)
            checks.push_back(std::async(std::launch::async, [&store] {
                return store.healthy();
            }));
        for (auto &check : checks)
            if (!check.get())
                throw std::runtime_error("concurrent pool health check failed");

        const auto pool = store.poolSnapshot();
        require(pool.size == 4 && pool.available == 4 && pool.inUse == 0,
                "pool gauges did not return to idle");
        require(pool.acquires > 0 && pool.inUseHighWatermark > 0 &&
                    pool.inUseHighWatermark <= pool.size,
                "pool acquisition counters are invalid");
        require(pool.timeouts == 0,
                "pool unexpectedly timed out during its contract test");

        std::cout << "phase11 PostgreSQL repository contract passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "phase11 PostgreSQL repository contract failed: "
                  << error.what() << '\n';
        return 1;
    }
}
