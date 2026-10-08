#include "server/Route/Router.h"
#include "server/http/RequestContext/RequestContext.h"
#include "server/phase11/business/AuthService/AuthService.h"
#include "server/phase11/business/ChatService/ChatService.h"
#include "server/phase11/business/FileService/FileService.h"
#include "server/phase11/business/SocialService/SocialService.h"
#include "server/phase11/ai/DeterministicKnowledgeIndex/DeterministicKnowledgeIndex.h"
#include "server/phase11/ai/RagService/RagService.h"
#include "server/phase11/security/InMemoryLoginRateLimiter/InMemoryLoginRateLimiter.h"
#include "server/phase11/storage/InMemoryStore/InMemoryStore.h"
#include "server/phase11/storage/files/LocalFileStorage/LocalFileStorage.h"
#include "server/phase11/training/TrainingDataService/TrainingDataService.h"
#include "server/phase11/transport/FlatJson/FlatJson.h"
#include "server/phase11/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"
#include "server/phase11/transport/http/Phase11FileHttp/Phase11FileHttp.h"
#include "server/phase11/transport/http/Phase11KnowledgeHttp/Phase11KnowledgeHttp.h"
#include "server/phase11/transport/http/Phase11SocialChatHttp/Phase11SocialChatHttp.h"
#include "server/phase11/transport/http/Phase11TrainingHttp/Phase11TrainingHttp.h"
#include "server/phase11/transport/realtime/Phase11Realtime/Phase11Realtime.h"
#include "server/response/StringBody.h"
#include "server/websocket/WebSocketDispatcher/WebSocketDispatcher.h"
#include "server/websocket/WebSocketSessionManager/WebSocketSessionManager.h"

#include <cstdlib>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>

using namespace webserver::phase11;
using namespace webserver::phase11::transport;

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

class TestOnlyCredentialCodec final : public ICredentialCodec
{
public:
    std::string hashPassword(std::string_view password) const override
    {
        return "test-password:" + std::string(password);
    }
    std::string dummyPasswordHash() const override
    {
        return "test-password:dummy";
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
        return "test-hash:" + std::string(token);
    }
    bool verifyToken(std::string_view encoded,
                     std::string_view token) const override
    {
        return encoded == hashToken(token);
    }

private:
    mutable std::uint64_t counter_ = 0;
};

class AwaitingOutboxSink final : public IOutboxEventSink
{
public:
    bool publish(const OutboxEvent &) override
    {
        {
            std::lock_guard lock(mutex_);
            ++published_;
        }
        changed_.notify_all();
        return true;
    }

    bool waitForPublish(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout,
                                [this] { return published_ > 0; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t published_ = 0;
};

void verifyOutboxCommitWakeup()
{
    InMemoryStore store;
    AwaitingOutboxSink sink;
    Phase11OutboxPump pump(store, sink);
    store.appendOutbox({0, "test.created", "test", 1, "{}",
                        std::chrono::system_clock::now(), {}});
    pump.notifyCommitted();
    CHECK(sink.waitForPublish(std::chrono::seconds(1)));
    pump.stop();
    const auto metrics = pump.snapshot();
    CHECK(metrics.wakeups == 1);
    CHECK(metrics.published == 1);
    CHECK(metrics.attempted == 1);
    CHECK(metrics.failures == 0);
}

std::string responseBody(const HttpResponse &response)
{
    const auto body = std::dynamic_pointer_cast<StringBody>(response.body);
    CHECK(body && body->buffer_);
    return {body->buffer_->peek(), body->buffer_->readableBytes()};
}

struct Reply
{
    int status = 0;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
};

Reply request(Router &router, std::string method, std::string path,
              std::string body = {}, std::string cookie = {},
              std::string csrf = {},
              std::unordered_map<std::string, std::string> headers = {})
{
    RequestContext context;
    HttpResponse response;
    context.response = &response;
    context.request.method = std::move(method);
    context.request.path = std::move(path);
    context.request.bodyData = std::move(body);
    context.request.bodySize = context.request.bodyData.size();
    context.request.headers = std::move(headers);
    if (!context.request.bodyData.empty() &&
        !context.request.headers.contains("content-type"))
        context.request.headers["content-type"] = "application/json; charset=utf-8";
    if (!cookie.empty())
        context.request.headers["cookie"] = std::move(cookie);
    if (!csrf.empty())
        context.request.headers["x-csrf-token"] = std::move(csrf);
    CHECK(router.handle(context));
    return {response.status, responseBody(response), response.headers};
}

std::string jsonStringField(std::string_view json, std::string_view field)
{
    const auto marker = "\"" + std::string(field) + "\":\"";
    const auto start = json.find(marker);
    CHECK(start != std::string_view::npos);
    const auto valueStart = start + marker.size();
    const auto end = json.find('"', valueStart);
    CHECK(end != std::string_view::npos);
    return std::string(json.substr(valueStart, end - valueStart));
}

std::uint64_t jsonUnsignedField(std::string_view json, std::string_view field)
{
    const auto marker = "\"" + std::string(field) + "\":";
    const auto start = json.find(marker);
    CHECK(start != std::string_view::npos);
    const auto valueStart = start + marker.size();
    const auto end = json.find_first_not_of("0123456789", valueStart);
    const auto text = json.substr(valueStart, end - valueStart);
    CHECK(!text.empty());
    return std::stoull(std::string(text));
}

void verifyFlatJsonBoundary()
{
    const auto object = parseFlatStringObject(
        R"({"name":"Pika\nChu","emoji":"\ud83d\ude80"})");
    CHECK(object.at("name") == "Pika\nChu");
    CHECK(object.at("emoji") == "\xF0\x9F\x9A\x80");
    CHECK(quoteJson("a\n\"b") == "\"a\\n\\\"b\"");

    bool rejected = false;
    try
    {
        (void)parseFlatStringObject(R"({"name":"a","name":"b"})");
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    CHECK(rejected);
}

void verifyHttpFlow()
{
    InMemoryStore store;
    TestOnlyCredentialCodec credentials;
    InMemoryLoginRateLimiter limiter;
    AuthService auth(store, store, credentials, limiter);
    SocialService social(store, store, store, store);
    ChatService chat(store, store, store, store, store, store, store);
    TrainingDataService training(store, store, store, store, store);
    DeterministicKnowledgeIndex knowledgeIndex;
    RagService rag(store, store, store, knowledgeIndex);
    const auto fileRoot = std::filesystem::temp_directory_path() /
        ("phase11-http-files-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    LocalFileStorage fileStorage(fileRoot);
    FileService files(store, store, store, fileStorage);
    DatabaseExecutor databaseExecutor(2, 32);
    BoundedExecutor fileExecutor(1, 8);
    BoundedExecutor knowledgeExecutor(1, 8);
    Phase11AuthHttpConfig config;
    config.secureCookie = false;
    Phase11AuthHttp adapter(auth, databaseExecutor, config);
    WebSocketSessionManager webSockets;
    Phase11SocialChatHttp socialChat(
        adapter, social, chat, databaseExecutor, webSockets);
    Phase11FileHttp fileHttp(adapter, files, chat, fileExecutor);
    Phase11KnowledgeHttp knowledgeHttp(
        adapter, rag, knowledgeExecutor, {512U * 1024U});
    Phase11TrainingHttp trainingHttp(adapter, training, databaseExecutor);
    Router router;
    adapter.registerRoutes(router);
    socialChat.registerRoutes(router);
    fileHttp.registerRoutes(router);
    knowledgeHttp.registerRoutes(router);
    trainingHttp.registerRoutes(router);

    const auto malformed = request(router, "POST", "/api/auth/register",
                                   R"({"username":"alice",})");
    CHECK(malformed.status == 400);

    const auto registered = request(
        router, "POST", "/api/auth/register",
        R"({"username":"Alice","password":"correct-password","displayName":"Alice A"})");
    CHECK(registered.status == 201);
    CHECK(registered.body.find("\"username\":\"alice\"") != std::string::npos);
    CHECK(registered.body.find("password") == std::string::npos);
    const auto aliceId = jsonUnsignedField(registered.body, "id");
    const auto bob = request(
        router, "POST", "/api/auth/register",
        R"({"username":"Bob","password":"correct-password","displayName":"Bob B"})");
    CHECK(bob.status == 201);
    const auto bobId = jsonUnsignedField(bob.body, "id");

    const auto badLogin = request(
        router, "POST", "/api/auth/login",
        R"({"username":"alice","password":"wrong-password"})");
    CHECK(badLogin.status == 401);

    const auto login = request(
        router, "POST", "/api/auth/login",
        R"({"username":"alice","password":"correct-password"})");
    CHECK(login.status == 200);
    const auto csrf = jsonStringField(login.body, "csrfToken");
    const auto setCookie = login.headers.at("Set-Cookie");
    CHECK(setCookie.find("HttpOnly") != std::string::npos);
    CHECK(setCookie.find("SameSite=Strict") != std::string::npos);
    CHECK(setCookie.find("Secure") == std::string::npos);
    const auto cookieEnd = setCookie.find(';');
    const auto cookie = setCookie.substr(0, cookieEnd);

    const auto restored = request(router, "GET", "/api/me", {}, cookie);
    CHECK(restored.status == 200);
    CHECK(jsonStringField(restored.body, "csrfToken") == csrf);

    const auto rejectedPatch = request(
        router, "PATCH", "/api/me", R"({"biography":"hello"})",
        cookie, "wrong-csrf");
    CHECK(rejectedPatch.status == 403);

    const auto patched = request(
        router, "PATCH", "/api/me",
        R"({"displayName":"Alice B","biography":"hello"})", cookie, csrf);
    CHECK(patched.status == 200);
    CHECK(patched.body.find("Alice B") != std::string::npos);
    CHECK(patched.body.find("hello") != std::string::npos);

    const auto friendRequest = request(
        router, "POST", "/api/friend-requests",
        "{\"receiverId\":\"" + std::to_string(bobId) + "\"}", cookie, csrf);
    CHECK(friendRequest.status == 201);
    const auto requestId = jsonUnsignedField(friendRequest.body, "id");

    const auto bobLogin = request(
        router, "POST", "/api/auth/login",
        R"({"username":"bob","password":"correct-password"})");
    CHECK(bobLogin.status == 200);
    const auto bobCsrf = jsonStringField(bobLogin.body, "csrfToken");
    const auto bobSetCookie = bobLogin.headers.at("Set-Cookie");
    const auto bobCookie = bobSetCookie.substr(0, bobSetCookie.find(';'));
    const auto accepted = request(
        router, "POST", "/api/friend-requests/" + std::to_string(requestId),
        R"({"accept":"true"})", bobCookie, bobCsrf);
    CHECK(accepted.status == 200);

    const auto conversation = request(
        router, "POST", "/api/conversations",
        "{\"kind\":\"direct\",\"peerId\":\"" +
            std::to_string(bobId) + "\"}", cookie, csrf);
    CHECK(conversation.status == 201);
    const auto conversationId = jsonUnsignedField(conversation.body, "id");

    User ai;
    ai.username = "http-training-ai";
    ai.displayName = "HTTP Training AI";
    ai.passwordHash = "disabled";
    ai.aiAccount = true;
    ai.createdAt = std::chrono::system_clock::now();
    ai = store.create(std::move(ai));
    const auto aiConversation = chat.createDirectConversation(
        aliceId, ai.id, std::chrono::system_clock::now());
    const auto aiReply = chat.sendText(
        ai.id, aiConversation.id, "http-training-ai-reply",
        "raw model response", std::chrono::system_clock::now());
    const auto trainingBody =
        "{\"sourceMessageId\":\"" + std::to_string(aiReply.message.id) +
        "\",\"sanitizedPrompt\":\"Explain a safe transaction\","
        "\"sanitizedResponse\":\"Commit related writes together.\","
        "\"consentGranted\":\"true\"}";
    CHECK(request(router, "POST", "/api/training/candidates", trainingBody,
                  cookie, "wrong-csrf").status == 403);
    const auto submittedTraining = request(
        router, "POST", "/api/training/candidates", trainingBody,
        cookie, csrf);
    CHECK(submittedTraining.status == 201);
    CHECK(submittedTraining.body.find("\"state\":\"submitted\"") !=
          std::string::npos);
    const auto trainingCandidateId = jsonUnsignedField(
        submittedTraining.body, "id");
    const auto listedTraining = request(
        router, "GET", "/api/training/candidates", {}, cookie);
    CHECK(listedTraining.status == 200 &&
          listedTraining.body.find("Explain a safe transaction") !=
              std::string::npos);
    const auto revokedTraining = request(
        router, "POST", "/api/training/candidates/" +
            std::to_string(trainingCandidateId) + "/revoke",
        {}, cookie, csrf);
    CHECK(revokedTraining.status == 200 &&
          revokedTraining.body.find("\"state\":\"revoked\"") !=
              std::string::npos);

    // RAG HTTP 入口必须经过同一套 Cookie/CSRF，文档登记完成后业务服务负责
    // 切片、索引；引用查询返回可供 UI 展示的来源与实际片段。
    CHECK(request(
        router, "POST", "/api/knowledge/documents",
        R"({"title":"Architecture notes","sourceName":"notes.txt","mediaType":"text/plain","content":"PostgreSQL stores durable chat history and the outbox records committed notifications."})",
        cookie, "wrong-csrf").status == 403);
    const auto knowledgeDocument = request(
        router, "POST", "/api/knowledge/documents",
        R"({"title":"Architecture notes","sourceName":"notes.txt","mediaType":"text/plain","content":"PostgreSQL stores durable chat history and the outbox records committed notifications."})",
        cookie, csrf);
    CHECK(knowledgeDocument.status == 201);
    CHECK(knowledgeDocument.body.find("\"state\":\"ready\"") !=
          std::string::npos);
    const auto listedKnowledge = request(
        router, "GET", "/api/knowledge/documents", {}, cookie);
    CHECK(listedKnowledge.status == 200);
    CHECK(listedKnowledge.body.find("Architecture notes") !=
          std::string::npos);
    const auto sharedKnowledgeDocument = request(
        router, "POST", "/api/knowledge/documents",
        "{\"title\":\"Shared architecture\",\"sourceName\":\"shared.txt\","
        "\"mediaType\":\"text/plain\",\"content\":\"PostgreSQL stores durable chat history and the outbox records committed notifications.\","
        "\"conversationId\":\"" + std::to_string(conversationId) + "\"}",
        cookie, csrf);
    CHECK(sharedKnowledgeDocument.status == 201);
    const auto ragContext = rag.retrieveForGeneration(
        "http-rag-generation", aliceId, conversationId,
        "Where is durable chat history stored?", 4,
        std::chrono::system_clock::now());
    CHECK(!ragContext.citations.empty());
    const auto citations = request(
        router, "GET",
        "/api/ai/generations/http-rag-generation/citations", {}, bobCookie);
    CHECK(citations.status == 200);
    CHECK(citations.body.find("Shared architecture") != std::string::npos);
    CHECK(citations.body.find("durable chat history") != std::string::npos);
    const auto persisted = chat.sendText(
        aliceId, conversationId, "http-test-message", "persistent hello",
        std::chrono::system_clock::now());
    CHECK(persisted.inserted);
    const auto history = request(
        router, "GET", "/api/conversations/" +
                           std::to_string(conversationId) + "/messages",
        {}, bobCookie);
    CHECK(history.status == 200);
    CHECK(history.body.find("persistent hello") != std::string::npos);
    CHECK(history.body.find("\"createdAt\":") != std::string::npos);
    const auto members = request(
        router, "GET", "/api/conversations/" +
                           std::to_string(conversationId) + "/members",
        {}, bobCookie);
    CHECK(members.status == 200);
    CHECK(members.body.find("\"userId\":" + std::to_string(aliceId)) !=
          std::string::npos);
    CHECK(members.body.find("\"userId\":" + std::to_string(bobId)) !=
          std::string::npos);
    CHECK(members.body.find("\"role\":\"member\"") != std::string::npos);
    CHECK(request(router, "GET", "/api/friends", {}, cookie).body.find(
              "\"username\":\"bob\"") != std::string::npos);
    const auto contactUpdate = request(
        router, "PATCH", "/api/friends/" + std::to_string(bobId),
        R"({"remark":"Study partner","notificationsMuted":"true"})",
        cookie, csrf);
    CHECK(contactUpdate.status == 200);
    CHECK(contactUpdate.body.find("\"remark\":\"Study partner\"") !=
          std::string::npos);
    CHECK(contactUpdate.body.find("\"notificationsMuted\":true") !=
          std::string::npos);

    // 文件 HTTP 流程与业务测试关注点不同：这里验证真实路由、Cookie/CSRF、原始
    // 二进制请求体和下载响应能连起来，而分片存储细节由 phase11_file_tests 负责。
    const std::string png =
        std::string("\x89PNG\r\n\x1a\n", 8) + "http-adapter";
    const auto createdFile = request(
        router, "POST", "/api/files/uploads",
        "{\"name\":\"http.png\",\"mediaType\":\"image/png\",\"size\":\"" +
            std::to_string(png.size()) + "\"}", cookie, csrf);
    CHECK(createdFile.status == 201);
    const auto fileId = jsonUnsignedField(createdFile.body, "id");
    const auto uploadedFile = request(
        router, "PATCH", "/api/files/" + std::to_string(fileId) + "/content",
        png, cookie, csrf,
        {{"content-type", "application/octet-stream"}, {"upload-offset", "0"}});
    CHECK(uploadedFile.status == 200 &&
          jsonUnsignedField(uploadedFile.body, "uploadedSize") == png.size());
    const auto completedFile = request(
        router, "POST", "/api/files/" + std::to_string(fileId) + "/complete",
        {}, cookie, csrf);
    CHECK(completedFile.status == 200 &&
          completedFile.body.find("\"state\":\"ready\"") != std::string::npos);
    const auto fileMessage = request(
        router, "POST", "/api/conversations/" +
            std::to_string(conversationId) + "/file-messages",
        "{\"clientMessageId\":\"http-file-message\",\"fileId\":\"" +
            std::to_string(fileId) + "\",\"caption\":\"HTTP file\"}",
        cookie, csrf);
    CHECK(fileMessage.status == 201 &&
          fileMessage.body.find("\"attachments\":[{") != std::string::npos);
    const auto downloaded = request(
        router, "GET", "/api/files/" + std::to_string(fileId), {}, bobCookie);
    CHECK(downloaded.status == 200 && downloaded.body == png);
    CHECK(downloaded.headers.at("Content-Type") == "image/png");
    CHECK(downloaded.headers.at("X-Content-Type-Options") == "nosniff");
    CHECK(downloaded.headers.at("Accept-Ranges") == "bytes");
    const auto avatar = request(
        router, "POST", "/api/me/avatar",
        "{\"fileId\":\"" + std::to_string(fileId) + "\"}", cookie, csrf);
    CHECK(avatar.status == 200 &&
          avatar.body.find("\"avatarUrl\":\"/api/files/") != std::string::npos);

    WebSocketDispatcher dispatcher;
    Phase11Realtime realtime(chat, databaseExecutor);
    realtime.registerHandlers(dispatcher);
    WsMessageContext send;
    send.uid = aliceId;
    send.inbound.type = "chat.send";
    send.inbound.messageId = "ws-client-message";
    send.inbound.conversationId = conversationId;
    send.inbound.text = "websocket persistent hello";
    CHECK(dispatcher.dispatch(send));
    CHECK(send.hasOutbound);
    CHECK(send.outbound.text.find("inserted") != std::string::npos);
    const auto sentSequence = jsonUnsignedField(send.outbound.text, "sequence");

    WsMessageContext duplicate;
    duplicate.uid = aliceId;
    duplicate.inbound = send.inbound;
    CHECK(dispatcher.dispatch(duplicate));
    CHECK(duplicate.outbound.text.find("duplicate") != std::string::npos);

    WsMessageContext ack;
    ack.uid = bobId;
    ack.inbound.type = "chat.ack";
    ack.inbound.conversationId = conversationId;
    ack.inbound.sequence = sentSequence;
    CHECK(dispatcher.dispatch(ack));
    CHECK(ack.outbound.text.find("accepted") != std::string::npos);

    CHECK(request(router, "POST", "/api/blocks/" + std::to_string(bobId),
                  {}, cookie, csrf).status == 200);
    CHECK(request(router, "GET", "/api/blocks", {}, cookie).body.find(
              "\"username\":\"bob\"") != std::string::npos);
    CHECK(request(router, "DELETE", "/api/blocks/" + std::to_string(bobId),
                  {}, cookie, csrf).status == 200);

    const auto logout = request(router, "POST", "/api/auth/logout",
                                {}, cookie, csrf);
    CHECK(logout.status == 200);
    CHECK(logout.headers.at("Set-Cookie").find("Max-Age=0") !=
          std::string::npos);
    CHECK(request(router, "GET", "/api/me", {}, cookie).status == 401);

    fileExecutor.shutdown();
    knowledgeExecutor.shutdown();
    databaseExecutor.shutdown();
    std::error_code ignored;
    std::filesystem::remove_all(fileRoot, ignored);
}

} // namespace

int main()
{
    verifyFlatJsonBoundary();
    verifyOutboxCommitWakeup();
    verifyHttpFlow();
    std::cout << "phase11 HTTP auth tests passed\n";
}
