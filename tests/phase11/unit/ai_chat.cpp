#include "server/phase11/ai/ModelRegistryService/ModelRegistryService.h"
#include "server/phase11/business/AiChatService/AiChatService.h"
#include "server/phase11/business/ChatService/ChatService.h"
#include "server/phase11/storage/InMemoryStore/InMemoryStore.h"
#include "server/phase11/transport/realtime/Phase11Realtime/Phase11Realtime.h"
#include "server/websocket/WebSocketDispatcher/WebSocketDispatcher.h"
#include "server/websocket/WebSocketDispatcher/WsMessageContext.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>

using namespace webserver::phase11;

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

class RecordingEvents final : public IAiGenerationEventSink
{
public:
    void token(const AiTokenEvent &event) override
    {
        std::lock_guard lock(mutex_);
        tokens[event.generationId].push_back(event);
    }

    void completed(const AiCompletedEvent &event) override
    {
        {
            std::lock_guard lock(mutex_);
            completions.insert_or_assign(event.generationId, event);
        }
        changed_.notify_all();
    }

    AiCompletedEvent waitFor(const std::string &id)
    {
        std::unique_lock lock(mutex_);
        CHECK(changed_.wait_for(lock, std::chrono::seconds(3), [&]
        {
            return completions.contains(id);
        }));
        return completions.at(id);
    }

    std::vector<AiTokenEvent> tokensFor(const std::string &id)
    {
        std::lock_guard lock(mutex_);
        return tokens[id];
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::unordered_map<std::string, std::vector<AiTokenEvent>> tokens;
    std::unordered_map<std::string, AiCompletedEvent> completions;
};

class AsyncScriptedProvider final : public IModelProvider,
                                    public IModelArtifactValidator
{
public:
    ~AsyncScriptedProvider() override
    {
        std::vector<std::thread> workers;
        {
            std::lock_guard lock(mutex_);
            workers.swap(workers_);
        }
        for (auto &worker : workers)
            if (worker.joinable())
                worker.join();
    }

    std::string name() const override { return "ollama"; }
    bool ready() const noexcept override { return true; }
    std::string supportedRuntime() const override { return name(); }
    ModelArtifactValidation validateVersion(const ModelVersion &) override
    {
        return {true, {}};
    }

    GenerationHandle generate(const GenerationRequest &request,
                              TokenSink onToken,
                              CompletionSink onComplete,
                              CancellationToken cancellation) override
    {
        {
            std::lock_guard lock(observationMutex_);
            observed = request;
        }
        std::thread worker(
            [request, onToken = std::move(onToken),
             onComplete = std::move(onComplete), cancellation]() mutable
            {
                const bool slow = !request.messages.empty() &&
                                  request.messages.back().content == "slow";
                if (slow)
                {
                    for (int attempt = 0;
                         attempt < 100 && !cancellation.cancelled(); ++attempt)
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    onComplete({false, cancellation.cancelled(),
                                cancellation.cancelled() ? "cancelled" : "timeout",
                                {}});
                    return;
                }
                onToken("hello ");
                onToken("phase11");
                onComplete({true, false, {}, {}});
            });
        {
            std::lock_guard lock(mutex_);
            workers_.push_back(std::move(worker));
        }
        return {request.requestId, cancellation};
    }

    GenerationRequest snapshot() const
    {
        std::lock_guard lock(observationMutex_);
        return observed;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::thread> workers_;
    mutable std::mutex observationMutex_;
    GenerationRequest observed;
};

User makeUser(InMemoryStore &store, std::string username, bool ai,
              TimePoint now)
{
    return store.create(
        {0, std::move(username), "Test User", {}, "disabled", false, ai, now});
}

WsMessageContext generateContext(UserId requester, UserId ai,
                                 ConversationId conversation,
                                 std::string id, std::string prompt)
{
    WsMessageContext context;
    context.uid = requester;
    context.inbound.type = "ai.generate";
    context.inbound.toUserId = ai;
    context.inbound.conversationId = conversation;
    context.inbound.messageId = std::move(id);
    context.inbound.text = std::move(prompt);
    return context;
}

} // namespace

int main()
{
    const auto now = TimePoint{std::chrono::seconds{10'000}};
    InMemoryStore store;
    ChatService chat(store, store, store, store, store, store);
    ModelRegistryService registry(store);
    ModelRouter router(store);
    DatabaseExecutor databaseExecutor(2, 32);
    RecordingEvents events;

    const auto alice = makeUser(store, "alice-ai-test", false, now);
    const auto ai = makeUser(store, "assistant-ai-test", true, now);
    const auto bob = makeUser(store, "bob-ai-test", false, now);
    const auto conversation = chat.createDirectConversation(alice.id, ai.id, now);
    const auto group = chat.createGroupConversation(
        alice.id, "validation", {bob.id}, now);

    const auto node = registry.createNode("ollama-assistant",
                                          ModelNodeKind::Specialist, now);
    const auto provider = std::make_shared<AsyncScriptedProvider>();
    registry.registerValidator(provider);
    const auto version = registry.registerVersion(
        node.id, "ollama", "qwen-learning", {}, "sha256:test", now);
    registry.activate(node.id, version.id);
    registry.bindAiAccount(ai.id, node.id);
    router.registerProvider(provider);

    AiChatService aiChat(chat, store, store, router, databaseExecutor, events);
    webserver::phase11::transport::Phase11Realtime realtime(
        chat, databaseExecutor, &aiChat);
    WebSocketDispatcher dispatcher;
    realtime.registerHandlers(dispatcher);

    auto success = generateContext(alice.id, ai.id, conversation.id,
                                   "generation-success", "hello model");
    CHECK(dispatcher.dispatch(success));
    CHECK(success.hasOutbound);
    CHECK(success.outbound.type == "ai.accepted");
    const auto completed = events.waitFor("generation-success");
    CHECK(completed.success && !completed.cancelled && completed.message);
    CHECK(completed.message->senderId == ai.id);
    CHECK(completed.message->body == "hello phase11");
    CHECK(completed.message->modelTrace.nodeId == node.id);
    CHECK(completed.message->modelTrace.versionId == version.id);
    CHECK(completed.message->modelTrace.adapterName.empty());

    const auto tokens = events.tokensFor("generation-success");
    CHECK(tokens.size() == 2);
    CHECK(tokens[0].tokenIndex == 1 && tokens[0].text == "hello ");
    CHECK(tokens[1].tokenIndex == 2 && tokens[1].text == "phase11");
    CHECK(tokens[0].recipients.size() == 2);

    const auto history = chat.history(alice.id, conversation.id, 0, 20);
    CHECK(history.size() == 2);
    CHECK(history[0].senderId == alice.id &&
          history[0].clientMessageId == "generation-success");
    CHECK(history[1].senderId == ai.id &&
          history[1].clientMessageId == "ai:generation-success");
    const auto observed = provider->snapshot();
    CHECK(observed.requester == alice.id);
    CHECK(observed.modelArtifact == "qwen-learning");
    CHECK(observed.adapterArtifact.empty());
    CHECK(!observed.messages.empty() &&
          observed.messages.back().content == "hello model");

    // 普通账号不能冒充 AI；错误通过异步完成事件返回，不阻塞 WebSocket Worker。
    auto invalid = generateContext(alice.id, bob.id, group.id,
                                   "generation-invalid", "hello");
    CHECK(dispatcher.dispatch(invalid));
    CHECK(invalid.outbound.type == "ai.accepted");
    const auto rejected = events.waitFor("generation-invalid");
    CHECK(!rejected.success && !rejected.cancelled);
    CHECK(rejected.recipients.size() == 1 &&
          rejected.recipients.front() == alice.id);
    CHECK(rejected.error.find("not an enabled AI") != std::string::npos);

    // 取消命令只能命中该用户自己的在途生成，并最终收到 cancelled 终态。
    auto slow = generateContext(alice.id, ai.id, conversation.id,
                                "generation-cancel", "slow");
    CHECK(dispatcher.dispatch(slow));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    WsMessageContext cancel;
    cancel.uid = alice.id;
    cancel.inbound.type = "ai.cancel";
    cancel.inbound.messageId = "cancel-command";
    cancel.inbound.replyTo = "generation-cancel";
    CHECK(dispatcher.dispatch(cancel));
    CHECK(cancel.outbound.type == "ai.cancel.accepted");
    const auto cancelled = events.waitFor("generation-cancel");
    CHECK(!cancelled.success && cancelled.cancelled);

    aiChat.shutdown();
    databaseExecutor.shutdown();
    return 0;
}
