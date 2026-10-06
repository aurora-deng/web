#include "server/phase11/storage/postgres/PostgresStore/PostgresStore.h"
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
        "TRUNCATE TABLE users, model_nodes, dataset_versions, outbox_events "
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
    const auto member = store.findMember(direct->id, bob->id);
    require(member && member->lastDeliveredSequence == 1 &&
                member->lastReadSequence == 1,
            "member cursors were not persisted");
    const auto messages = store.messagesAfter(direct->id, 0, 10);
    require(messages.size() == 1 &&
                store.findMessage(messages.front().id).has_value() &&
                store.searchMessages(direct->id, "ell", 10).size() == 1,
            "message lookup or search failed");

    User charlie{0, "contract-charlie", "Charlie", {}, "hash-c",
                 false, false, now};
    charlie = store.create(std::move(charlie));
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
    require(store.activateModelVersion(specialist.id, specialistVersion.id),
            "valid model activation failed");
    require(!store.activateModelVersion(specialist.id, rootVersion.id),
            "version belonging to another node was activated");
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

    auto candidate = store.createTrainingCandidate(
        {0, alice->id, messages.front().id, "prompt", "response",
         "consent-v1", TrainingCandidateState::Submitted, {}, now});
    candidate.state = TrainingCandidateState::Approved;
    require(store.updateTrainingCandidate(candidate),
            "training candidate review update failed");
    const auto dataset = store.createDatasetVersion(
        {0, "contract-dataset", "sha256:dataset", {candidate.id}, now});
    candidate.state = TrainingCandidateState::Exported;
    candidate.datasetVersionId = dataset.id;
    require(store.updateTrainingCandidate(candidate),
            "training candidate export update failed");
    const auto exported = store.findTrainingCandidate(candidate.id);
    require(exported && exported->state == TrainingCandidateState::Exported &&
                exported->datasetVersionId == dataset.id,
            "training candidate export state was not persisted");
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
