#include "server/Route/Router.h"
#include "server/http/RequestContext/RequestContext.h"
#include "server/phase11/business/AuthService/AuthService.h"
#include "server/phase11/business/ChatService/ChatService.h"
#include "server/phase11/business/SocialService/SocialService.h"
#include "server/phase11/security/InMemoryLoginRateLimiter/InMemoryLoginRateLimiter.h"
#include "server/phase11/storage/InMemoryStore/InMemoryStore.h"
#include "server/phase11/transport/FlatJson/FlatJson.h"
#include "server/phase11/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"
#include "server/phase11/transport/http/Phase11SocialChatHttp/Phase11SocialChatHttp.h"
#include "server/phase11/transport/realtime/Phase11Realtime/Phase11Realtime.h"
#include "server/response/StringBody.h"
#include "server/websocket/WebSocketDispatcher/WebSocketDispatcher.h"

#include <cstdlib>
#include <iostream>
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
              std::string csrf = {})
{
    RequestContext context;
    HttpResponse response;
    context.response = &response;
    context.request.method = std::move(method);
    context.request.path = std::move(path);
    context.request.bodyData = std::move(body);
    context.request.bodySize = context.request.bodyData.size();
    if (!context.request.bodyData.empty())
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
    ChatService chat(store, store, store, store, store, store);
    DatabaseExecutor databaseExecutor(2, 32);
    Phase11AuthHttpConfig config;
    config.secureCookie = false;
    Phase11AuthHttp adapter(auth, databaseExecutor, config);
    Phase11SocialChatHttp socialChat(adapter, social, chat, databaseExecutor);
    Router router;
    adapter.registerRoutes(router);
    socialChat.registerRoutes(router);

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

    const auto logout = request(router, "POST", "/api/auth/logout",
                                {}, cookie, csrf);
    CHECK(logout.status == 200);
    CHECK(logout.headers.at("Set-Cookie").find("Max-Age=0") !=
          std::string::npos);
    CHECK(request(router, "GET", "/api/me", {}, cookie).status == 401);

    databaseExecutor.shutdown();
}

} // namespace

int main()
{
    verifyFlatJsonBoundary();
    verifyHttpFlow();
    std::cout << "phase11 HTTP auth tests passed\n";
}
