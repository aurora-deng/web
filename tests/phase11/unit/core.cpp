#include "server/phase11/ai/ModelRegistryService/ModelRegistryService.h"
#include "server/phase11/business/AuthService/AuthService.h"
#include "server/phase11/business/ChatService/ChatService.h"
#include "server/phase11/business/OutboxDispatcher/OutboxDispatcher.h"
#include "server/phase11/business/SocialService/SocialService.h"
#include "server/phase11/storage/InMemoryStore/InMemoryStore.h"
#include "server/phase11/runtime/BoundedExecutor/BoundedExecutor.h"
#include "server/phase11/security/InMemoryLoginRateLimiter/InMemoryLoginRateLimiter.h"
#include "server/phase11/transport/Contracts/Contracts.h"
#include "server/phase11/training/TrainingDataService/TrainingDataService.h"
#include "tests/phase11/unit/RepositoryContract.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sstream>

using namespace webserver::phase11;

// 标准 assert 在 Release/NDEBUG 下会连表达式一起删除。CHECK 始终执行，保证两种构建
// 真正验证相同流程；失败时打印位置并终止测试进程。
#define CHECK(expression)                                                       \
    do                                                                          \
    {                                                                           \
        if (!(expression))                                                      \
        {                                                                       \
            std::cerr << "CHECK failed: " #expression << " at " << __FILE__    \
                      << ':' << __LINE__ << '\n';                               \
            std::abort();                                                       \
        }                                                                       \
    } while (false)

namespace
{

/** 测试专用编码器。它刻意不进入生产库，防止弱散列被误用于真实密码。 */
class TestOnlyCredentialCodec final : public ICredentialCodec
{
public:
    std::string hashPassword(std::string_view password) const override
    {
        return "test-password:" + std::string(password);
    }

    std::string dummyPasswordHash() const override
    {
        return "test-password:dummy-password";
    }

    bool verifyPassword(std::string_view encoded,
                        std::string_view password) const override
    {
        return encoded == hashPassword(password);
    }

    std::string randomToken() const override
    {
        return "test-token-" + std::to_string(++counter_);
    }

    std::string hashToken(std::string_view token) const override
    {
        return "test-token-hash:" + std::string(token);
    }

    bool verifyToken(std::string_view encoded,
                     std::string_view token) const override
    {
        return encoded == hashToken(token);
    }

private:
    mutable std::uint64_t counter_ = 0;
};

class ScriptedProvider final : public IModelProvider,
                               public IModelArtifactValidator
{
public:
    std::string name() const override { return "onnx-inprocess"; }
    bool ready() const noexcept override { return true; }
    std::string supportedRuntime() const override { return name(); }

    ModelArtifactValidation validateVersion(
        const ModelVersion &version) override
    {
        ++validationCount;
        if (version.checksum == rejectedChecksum)
            return {false, "simulated incompatible adapter"};
        return {true, {}};
    }

    GenerationHandle generate(const GenerationRequest &request,
                              TokenSink onToken,
                              CompletionSink onComplete,
                              CancellationToken cancellation) override
    {
        selectedModel = request.modelArtifact;
        selectedAdapter = request.adapterArtifact;
        if (!cancellation.cancelled())
        {
            onToken("phase");
            onToken("11");
            onComplete({true, false, {}, {}});
        }
        else
        {
            onComplete({false, true, {}, {}});
        }
        return {request.requestId, cancellation};
    }

    std::string selectedModel;
    std::string selectedAdapter;
    std::string rejectedChecksum = "sha256:incompatible";
    std::size_t validationCount = 0;
};

class RecordingExporter final : public ITrainingDataExporter
{
public:
    std::string exportDataset(
        const DatasetVersion &dataset,
        const std::vector<TrainingCandidate> &candidates) override
    {
        datasetId = dataset.id;
        candidateCount = candidates.size();
        return "memory://dataset/" + std::to_string(dataset.id);
    }

    DatasetVersionId datasetId = 0;
    std::size_t candidateCount = 0;
};

class ThrowingOutbox final : public IOutboxRepository
{
public:
    explicit ThrowingOutbox(InMemoryStore &store) : store_(store) {}

    OutboxEvent appendOutbox(OutboxEvent) override
    {
        throw std::runtime_error("simulated outbox write failure");
    }

    std::vector<OutboxEvent> pendingOutbox(std::size_t limit) const override
    {
        return store_.pendingOutbox(limit);
    }

    bool markPublished(OutboxEventId id, TimePoint now) override
    {
        return store_.markPublished(id, now);
    }

private:
    InMemoryStore &store_;
};

class RecordingOutboxSink final : public IOutboxEventSink
{
public:
    bool publish(const OutboxEvent &event) override
    {
        attempted.push_back(event.id);
        if (failNext)
        {
            failNext = false;
            return false;
        }
        accepted.push_back(event.id);
        return true;
    }

    bool failNext = false;
    std::vector<OutboxEventId> attempted;
    std::vector<OutboxEventId> accepted;
};

template<class F>
void expectError(ErrorCode expected, F &&operation)
{
    try
    {
        operation();
        CHECK(false && "expected ApplicationError");
    }
    catch (const ApplicationError &error)
    {
        CHECK(error.code() == expected);
    }
}
struct Fixture
{
    Fixture()
        : auth(store, store, credentials, rateLimiter),
          social(store, store, store, store),
          chat(store, store, store, store, store, store),
          models(store), router(store), training(store, store, store, store)
    {
    }

    InMemoryStore store;
    TestOnlyCredentialCodec credentials;
    InMemoryLoginRateLimiter rateLimiter;
    AuthService auth;
    SocialService social;
    ChatService chat;
    ModelRegistryService models;
    ModelRouter router;
    TrainingDataService training;
};

void verifyLoginRateLimit(TimePoint now)
{
    InMemoryStore store;
    TestOnlyCredentialCodec credentials;
    InMemoryLoginRateLimiter limiter(
        {2, 10, std::chrono::minutes(5), std::chrono::minutes(15)});
    AuthService auth(store, store, credentials, limiter);
    auth.registerUser("limited-user", "correct-password", "Limited", now);

    for (int attempt = 0; attempt < 2; ++attempt)
        expectError(ErrorCode::Unauthenticated, [&] {
            auth.login("limited-user", "wrong-password", now, "127.0.0.1");
        });
    expectError(ErrorCode::RateLimited, [&] {
        auth.login("limited-user", "correct-password", now, "127.0.0.1");
    });

    // 封禁时间过后恢复；成功登录会清空账号失败桶。
    const auto recovered = auth.login(
        "limited-user", "correct-password", now + std::chrono::minutes(16),
        "127.0.0.1");
    CHECK(recovered.user.username == "limited-user");
}

void verifyTransactionRollback()
{
    InMemoryStore store;
    {
        auto transaction = store.beginTransaction();
        store.create({0, "rollback-user", "Rollback", {}, "hash",
                      false, false, {}});
        // 未提交离开作用域，应恢复事务开始前的快照。
    }
    CHECK(!store.findUserByName("rollback-user"));
}

void verifyReusableRepositoryContract(TimePoint now)
{
    InMemoryStore store;
    test::runRepositoryContract(
        {store, store, store, store, store, store, store}, now);
}

void verifyOutboxRetry(TimePoint now)
{
    InMemoryStore store;
    store.appendOutbox(
        {0, "first", "test", 1, "1", now, {}});
    store.appendOutbox(
        {0, "second", "test", 2, "2", now, {}});
    RecordingOutboxSink sink;
    OutboxDispatcher dispatcher(store, sink);

    sink.failNext = true;
    const auto blocked = dispatcher.dispatchPending(10, now);
    CHECK(blocked.attempted == 1 && blocked.published == 0 && blocked.blocked);
    CHECK(store.pendingOutbox(10).size() == 2);

    const auto retried = dispatcher.dispatchPending(10, now);
    CHECK(retried.attempted == 2 && retried.published == 2 && !retried.blocked);
    CHECK(store.pendingOutbox(10).empty());
    CHECK(sink.attempted.size() == 3 && sink.attempted[0] == sink.attempted[1]);
}

void verifyDatabaseExecutorBackpressure()
{
    std::mutex mutex;
    std::condition_variable started;
    std::condition_variable release;
    bool firstStarted = false;
    bool mayFinish = false;
    std::atomic<int> completed = 0;

    DatabaseExecutor executor(1, 2);
    CHECK(executor.submit([&] {
        std::unique_lock lock(mutex);
        firstStarted = true;
        started.notify_one();
        release.wait(lock, [&] { return mayFinish; });
        ++completed;
    }));
    {
        std::unique_lock lock(mutex);
        started.wait(lock, [&] { return firstStarted; });
    }

    // 一个任务正在执行、两个任务排队后，新任务必须被背压拒绝。第一个排队任务
    // 故意抛异常；Worker 应截住异常并继续执行第二个排队任务。
    CHECK(executor.submit([] { throw std::runtime_error("task failed"); }));
    CHECK(executor.submit([&] { ++completed; }));
    CHECK(executor.queued() == 2);
    CHECK(!executor.submit([&] { ++completed; }));
    {
        std::lock_guard lock(mutex);
        mayFinish = true;
    }
    release.notify_one();
    executor.shutdown();
    CHECK(completed == 2);
    CHECK(!executor.accepting());
    CHECK(!executor.submit([] {}));
}

void verifyAuthAndSocialFlow(Fixture &fixture, TimePoint now,
                             User &alice, User &bob)
{
    alice = fixture.auth.registerUser("Alice_01", "password-a", "Alice", now);
    bob = fixture.auth.registerUser("bob_02", "password-b", "Bob", now);
    CHECK(alice.username == "alice_01");

    const auto login = fixture.auth.login("ALICE_01", "password-a", now);
    CHECK(login.user.id == alice.id);
    CHECK(fixture.auth.authenticate(login.sessionToken, now));
    CHECK(fixture.auth.verifyCsrf(login.sessionToken, login.csrfToken, now));
    CHECK(!fixture.auth.verifyCsrf(login.sessionToken, "wrong", now));
    CHECK(!fixture.auth.authenticate(login.sessionToken,
                                      now + std::chrono::hours(24)));
    fixture.auth.logout(login.sessionToken, now);
    CHECK(!fixture.auth.authenticate(login.sessionToken, now));

    const auto request = fixture.social.requestFriendship(alice.id, bob.id, now);
    expectError(ErrorCode::Conflict, [&] {
        fixture.social.requestFriendship(alice.id, bob.id, now);
    });
    expectError(ErrorCode::Conflict, [&] {
        fixture.social.requestFriendship(bob.id, alice.id, now);
    });
    const auto accepted = fixture.social.decideFriendship(
        request.id, bob.id, true, now);
    CHECK(accepted.state == FriendRequestState::Accepted);
    CHECK(fixture.store.areFriends(alice.id, bob.id));
    const auto friends = fixture.social.friends(alice.id);
    CHECK(friends.size() == 1 && friends.front().id == bob.id);
}

Message verifyChatFlow(Fixture &fixture, TimePoint now,
                       const User &alice, const User &bob)
{
    const auto direct = fixture.chat.createDirectConversation(
        alice.id, bob.id, now);
    CHECK(fixture.chat.createDirectConversation(alice.id, bob.id, now).id ==
           direct.id);
    CHECK(fixture.chat.conversations(alice.id).size() == 1);
    const auto participants = fixture.chat.members(alice.id, direct.id);
    CHECK(participants.size() == 2);
    CHECK(std::any_of(participants.begin(), participants.end(),
                      [&bob](const auto &participant)
                      {
                          return participant.user.id == bob.id &&
                                 participant.user.displayName == "Bob" &&
                                 participant.membership.userId == bob.id;
                      }));

    const auto first = fixture.chat.sendText(
        alice.id, direct.id, "client-1", "hello phase11", now);
    CHECK(first.inserted && first.message.sequence == 1);
    const auto duplicate = fixture.chat.sendText(
        alice.id, direct.id, "client-1", "hello phase11", now);
    CHECK(!duplicate.inserted && duplicate.message.id == first.message.id);
    expectError(ErrorCode::Conflict, [&] {
        fixture.chat.sendText(alice.id, direct.id, "client-1",
                              "different body", now);
    });

    const auto history = fixture.chat.history(bob.id, direct.id, 0, 20);
    CHECK(history.size() == 1 && history.front().body == "hello phase11");
    CHECK(fixture.chat.search(bob.id, direct.id, "phase11", 10).size() == 1);
    fixture.chat.acknowledgeDelivery(bob.id, direct.id, 1);
    fixture.chat.markRead(bob.id, direct.id, 1);
    const auto bobState = fixture.store.findMember(direct.id, bob.id);
    CHECK(bobState && bobState->lastDeliveredSequence == 1 &&
           bobState->lastReadSequence == 1);

    const auto group = fixture.chat.createGroupConversation(
        alice.id, "Study", {bob.id}, now);
    const auto charlie = fixture.auth.registerUser(
        "charlie", "password-c", "Charlie", now);
    expectError(ErrorCode::Forbidden, [&] {
        fixture.chat.addGroupMember(bob.id, group.id, charlie.id, now);
    });
    fixture.chat.addGroupMember(alice.id, group.id, charlie.id, now);
    CHECK(fixture.store.findMember(group.id, charlie.id));

    // 业务层不要求接收方存在 WebSocket/SSE 会话。这里没有创建任何实时连接，
    // 仍应允许 Alice 单向写入私聊和群聊；Bob/Charlie 稍后查询历史即可恢复。
    const auto groupOffline = fixture.chat.sendText(
        alice.id, group.id, "group-offline-1", "members may be offline", now);
    CHECK(groupOffline.inserted && groupOffline.message.sequence == 1);
    CHECK(fixture.chat.history(bob.id, group.id, 0, 20).size() == 1);
    CHECK(fixture.chat.history(charlie.id, group.id, 0, 20).size() == 1);

    // 前面的好友申请，加上 direct conversation + message + group + member-added 和
    // 离线群消息共六条
    // Outbox；重放同一消息不会再写一条 message.created。
    CHECK(fixture.store.pendingOutbox(20).size() == 6);
    return first.message;
}

void verifyMessageOutboxAtomicity(Fixture &fixture, TimePoint now,
                                  const User &alice, const User &bob)
{
    const auto direct = fixture.store.findDirectConversation(alice.id, bob.id);
    CHECK(direct);
    const auto before = fixture.store.findConversation(direct->id);
    CHECK(before);

    ThrowingOutbox failingOutbox(fixture.store);
    ChatService failingChat(fixture.store, fixture.store, fixture.store,
                            fixture.store, fixture.store, failingOutbox);
    try
    {
        (void)failingChat.sendText(alice.id, direct->id, "rollback-message",
                                   "must not survive", now);
        CHECK(false && "expected simulated outbox failure");
    }
    catch (const std::runtime_error &)
    {
    }

    const auto after = fixture.store.findConversation(direct->id);
    CHECK(after && after->nextSequence == before->nextSequence);
    CHECK(fixture.store.searchMessages(direct->id, "must not survive", 10).empty());
}

User verifyModelFlow(Fixture &fixture, TimePoint now)
{
    User ai;
    ai.username = "phase11-ai";
    ai.displayName = "Phase 11 AI";
    ai.passwordHash = "disabled";
    ai.aiAccount = true;
    ai.createdAt = now;
    ai = fixture.store.create(std::move(ai));

    const auto root = fixture.models.createNode("root", ModelNodeKind::Root, now);
    const auto specialist = fixture.models.createNode(
        "cpp-specialist", ModelNodeKind::Specialist, now);
    fixture.models.connect(root.id, specialist.id,
                           ModelEdgeKind::ParentControlsChild);
    expectError(ErrorCode::Conflict, [&] {
        fixture.models.connect(specialist.id, root.id,
                               ModelEdgeKind::ParentControlsChild);
    });
    const auto provider = std::make_shared<ScriptedProvider>();
    fixture.models.registerValidator(provider);
    const auto version = fixture.models.registerVersion(
        specialist.id, "onnx-inprocess", "models/base", "cpp.onnx_adapter",
        "sha256:test", now);
    fixture.models.activate(specialist.id, version.id);

    // 不兼容 Adapter 在数据库切换前就被挡住，当前可用版本不能被退休。
    const auto incompatible = fixture.models.registerVersion(
        specialist.id, "onnx-inprocess", "models/base-v2",
        "broken.onnx_adapter", "sha256:incompatible", now);
    expectError(ErrorCode::Conflict, [&] {
        fixture.models.activate(specialist.id, incompatible.id);
    });
    auto activeNode = fixture.store.findModelNode(specialist.id);
    auto oldVersion = fixture.store.findModelVersion(version.id);
    CHECK(activeNode && activeNode->activeVersionId == version.id);
    CHECK(oldVersion && oldVersion->state == ModelVersionState::Active);

    // 新版本通过体检后切换；显式 rollback 会重新体检旧版本，再恢复 Active。
    const auto replacement = fixture.models.registerVersion(
        specialist.id, "onnx-inprocess", "models/base-v3",
        "cpp-v3.onnx_adapter", "sha256:v3", now);
    fixture.models.activate(specialist.id, replacement.id);
    oldVersion = fixture.store.findModelVersion(version.id);
    CHECK(oldVersion && oldVersion->state == ModelVersionState::Retired);
    fixture.models.rollback(specialist.id, version.id);
    activeNode = fixture.store.findModelNode(specialist.id);
    const auto replacementAfterRollback =
        fixture.store.findModelVersion(replacement.id);
    CHECK(activeNode && activeNode->activeVersionId == version.id);
    CHECK(replacementAfterRollback &&
          replacementAfterRollback->state == ModelVersionState::Retired);
    CHECK(provider->validationCount == 4);
    fixture.models.bindAiAccount(ai.id, specialist.id);

    fixture.router.registerProvider(provider);
    CHECK(fixture.router.modelReady(ai.id));
    std::string generated;
    GenerationCompletion completion;
    fixture.router.generate(
        ai.id,
        {"generation-1", ai.id, 0, {{"user", "hello"}}, 32, {}, {}, {}},
        [&](std::string_view token) { generated += token; },
        [&](GenerationCompletion result) { completion = std::move(result); });
    CHECK(generated == "phase11");
    CHECK(provider->selectedModel == "models/base");
    CHECK(provider->selectedAdapter == "cpp.onnx_adapter");
    CHECK(completion.success && completion.trace.nodeId == specialist.id &&
           completion.trace.versionId == version.id &&
           completion.trace.adapterName == "cpp.onnx_adapter");
    return ai;
}

void verifyTrainingConsent(Fixture &fixture, TimePoint now,
                           const User &owner, const Message &source)
{
    expectError(ErrorCode::Forbidden, [&] {
        fixture.training.submit(
            {owner.id, source.id, "prompt", "response", "v1", false}, now);
    });
    auto candidate = fixture.training.submit(
        {owner.id, source.id, "sanitized prompt", "sanitized response",
         "consent-v1", true}, now);

    const auto outsider = fixture.auth.registerUser(
        "outsider", "password-d", "Outsider", now);
    expectError(ErrorCode::Forbidden, [&] {
        fixture.training.submit(
            {outsider.id, source.id, "prompt", "response", "consent-v1", true},
            now);
    });
    candidate = fixture.training.review(candidate.id, true);
    CHECK(candidate.state == TrainingCandidateState::Approved);

    RecordingExporter exporter;
    const auto dataset = fixture.training.createDataset(
        "phase11-dataset-v1", {candidate.id}, "sha256:dataset", now, exporter);
    CHECK(exporter.datasetId == dataset.id && exporter.candidateCount == 1);
    const auto exported = fixture.store.findTrainingCandidate(candidate.id);
    CHECK(exported && exported->state == TrainingCandidateState::Exported &&
           exported->datasetVersionId == dataset.id);
}

} // namespace

int main()
{
    verifyTransactionRollback();
    verifyDatabaseExecutorBackpressure();
    static_assert(transport::kChatSend == std::string_view{"chat.send"});
    const auto now = TimePoint{std::chrono::seconds{1'000}};
    verifyLoginRateLimit(now);
    verifyReusableRepositoryContract(now);
    verifyOutboxRetry(now);
    Fixture fixture;
    User alice;
    User bob;
    verifyAuthAndSocialFlow(fixture, now, alice, bob);
    const auto message = verifyChatFlow(fixture, now, alice, bob);
    verifyMessageOutboxAtomicity(fixture, now, alice, bob);
    (void)verifyModelFlow(fixture, now);
    verifyTrainingConsent(fixture, now, alice, message);
    return 0;
}

\n