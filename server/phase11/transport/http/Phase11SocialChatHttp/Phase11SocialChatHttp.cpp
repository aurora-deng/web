#include "server/phase11/transport/http/Phase11SocialChatHttp/Phase11SocialChatHttp.h"

#include "server/Route/Router.h"
#include "server/http/RequestContext/RequestContext.h"
#include "server/phase11/transport/FlatJson/FlatJson.h"
#include "server/websocket/WebSocketSessionManager/WebSocketSessionManager.h"

#include <charconv>
#include <chrono>
#include <stdexcept>
#include <string_view>

namespace webserver::phase11::transport
{
namespace
{

TimePoint currentTime()
{
    return std::chrono::system_clock::now();
}

std::string statusText(int status)
{
    switch (status)
    {
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 429: return "Too Many Requests";
    case 503: return "Service Unavailable";
    default: return status >= 500 ? "Internal Server Error" : "OK";
    }
}

std::pair<int, std::string_view> mapError(ErrorCode code)
{
    switch (code)
    {
    case ErrorCode::InvalidArgument: return {400, "invalid_argument"};
    case ErrorCode::NotFound: return {404, "not_found"};
    case ErrorCode::Conflict: return {409, "conflict"};
    case ErrorCode::Unauthenticated: return {401, "unauthenticated"};
    case ErrorCode::Forbidden: return {403, "forbidden"};
    case ErrorCode::RateLimited: return {429, "rate_limited"};
    case ErrorCode::Unavailable: return {503, "unavailable"};
    }
    return {500, "internal"};
}

void reply(RequestContext &context, int status, std::string body)
{
    context.response->status = status;
    context.response->statusText = statusText(status);
    context.response->json(body);
    context.response->setHeader("Cache-Control", "no-store");
}

void error(RequestContext &context, int status,
           std::string_view code, std::string_view message)
{
    reply(context, status,
          "{\"code\":" + quoteJson(code) +
              ",\"message\":" + quoteJson(message) + "}");
}

template<class Operation>
bool guarded(RequestContext &context, Operation &&operation)
{
    try
    {
        operation();
    }
    catch (const ApplicationError &failure)
    {
        const auto [status, code] = mapError(failure.code());
        error(context, status, code, failure.what());
    }
    catch (const std::exception &)
    {
        error(context, 500, "internal", "internal server error");
    }
    return true;
}

FlatStringObject bodyObject(const RequestContext &context)
{
    if (context.request.bodyData.size() > 16 * 1024)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "JSON request body is too large");
    const auto contentType = context.request.headers.find("content-type");
    if (contentType == context.request.headers.end() ||
        !std::string_view(contentType->second).starts_with("application/json"))
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "Content-Type must be application/json");
    try
    {
        return parseFlatStringObject(context.request.bodyData);
    }
    catch (const std::invalid_argument &failure)
    {
        throw ApplicationError(ErrorCode::InvalidArgument, failure.what());
    }
}

std::string required(const FlatStringObject &fields, std::string_view name)
{
    const auto found = fields.find(std::string(name));
    if (found == fields.end())
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "missing field: " + std::string(name));
    return found->second;
}

std::uint64_t unsignedValue(std::string_view value, std::string_view field)
{
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(),
                                        result);
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size() || result == 0)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid positive integer: " + std::string(field));
    return result;
}

std::uint64_t nonnegativeValue(std::string_view value, std::string_view field)
{
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(),
                                        result);
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size())
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid non-negative integer: " +
                                   std::string(field));
    return result;
}

std::uint64_t optionalUnsigned(const RequestContext &context,
                               std::string_view field,
                               std::uint64_t fallback)
{
    const auto found = context.request.querryParams.find(std::string(field));
    if (found == context.request.querryParams.end())
        return fallback;
    std::uint64_t result = 0;
    const auto &value = found->second;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(),
                                        result);
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size())
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid integer: " + std::string(field));
    return result;
}

std::vector<UserId> idList(std::string_view input)
{
    std::vector<UserId> result;
    std::size_t start = 0;
    while (start < input.size())
    {
        const auto end = input.find(',', start);
        const auto part = input.substr(start,
            end == std::string_view::npos ? input.size() - start : end - start);
        result.push_back(unsignedValue(part, "memberIds"));
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return result;
}

std::string userJson(const User &user)
{
    return "{\"id\":" + std::to_string(user.id) +
           ",\"username\":" + quoteJson(user.username) +
           ",\"displayName\":" + quoteJson(user.displayName) +
           ",\"biography\":" + quoteJson(user.biography) +
           ",\"aiAccount\":" + (user.aiAccount ? "true" : "false") + "}";
}

std::string friendRequestJson(const FriendRequest &request)
{
    const char *state = request.state == FriendRequestState::Pending
                            ? "pending"
                            : request.state == FriendRequestState::Accepted
                                  ? "accepted" : "rejected";
    return "{\"id\":" + std::to_string(request.id) +
           ",\"senderId\":" + std::to_string(request.senderId) +
           ",\"receiverId\":" + std::to_string(request.receiverId) +
           ",\"state\":" + quoteJson(state) + "}";
}

std::int64_t epochMilliseconds(TimePoint value);

std::string contactJson(const FriendContact &contact,
                        std::size_t connections)
{
    const auto lastActive = contact.lastActiveAt
        ? std::to_string(epochMilliseconds(*contact.lastActiveAt)) : "null";
    return "{\"id\":" + std::to_string(contact.user.id) +
           ",\"username\":" + quoteJson(contact.user.username) +
           ",\"displayName\":" + quoteJson(contact.user.displayName) +
           ",\"biography\":" + quoteJson(contact.user.biography) +
           ",\"aiAccount\":" +
           (contact.user.aiAccount ? "true" : "false") +
           ",\"remark\":" + quoteJson(contact.settings.remark) +
           ",\"notificationsMuted\":" +
           (contact.settings.notificationsMuted ? "true" : "false") +
           ",\"online\":" + (connections ? "true" : "false") +
           ",\"connections\":" + std::to_string(connections) +
           ",\"lastActiveAt\":" + lastActive + "}";
}

std::string conversationJson(const Conversation &conversation)
{
    const auto state = conversation.state == ConversationState::Active
                           ? "active" : "dissolved";
    return "{\"id\":" + std::to_string(conversation.id) +
           ",\"kind\":" +
           quoteJson(conversation.kind == ConversationKind::Direct
                         ? "direct" : "group") +
           ",\"title\":" + quoteJson(conversation.title) +
           ",\"announcement\":" + quoteJson(conversation.announcement) +
           ",\"state\":" + quoteJson(state) +
           ",\"createdBy\":" + std::to_string(conversation.createdBy) +
           ",\"nextSequence\":" +
           std::to_string(conversation.nextSequence) + "}";
}

std::string conversationSummaryJson(const ConversationSummary &summary)
{
    const auto &conversation = summary.conversation;
    return "{\"id\":" + std::to_string(conversation.id) +
           ",\"kind\":" +
           quoteJson(conversation.kind == ConversationKind::Direct
                         ? "direct" : "group") +
           ",\"title\":" + quoteJson(conversation.title) +
           ",\"announcement\":" + quoteJson(conversation.announcement) +
           ",\"state\":" +
           quoteJson(conversation.state == ConversationState::Active
                         ? "active" : "dissolved") +
           ",\"createdBy\":" + std::to_string(conversation.createdBy) +
           ",\"nextSequence\":" + std::to_string(conversation.nextSequence) +
           ",\"lastDeliveredSequence\":" +
           std::to_string(summary.lastDeliveredSequence) +
           ",\"lastReadSequence\":" +
           std::to_string(summary.lastReadSequence) +
           ",\"unreadCount\":" + std::to_string(summary.unreadCount) + "}";
}

std::int64_t epochMilliseconds(TimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               value.time_since_epoch()).count();
}

std::string memberJson(const ConversationParticipant &participant)
{
    const auto &member = participant.membership;
    const auto &user = participant.user;
    return "{\"userId\":" + std::to_string(member.userId) +
           ",\"username\":" + quoteJson(user.username) +
           ",\"displayName\":" + quoteJson(user.displayName) +
           ",\"aiAccount\":" + (user.aiAccount ? "true" : "false") +
           ",\"role\":" +
           quoteJson(member.role == MemberRole::Owner ? "owner" :
                     member.role == MemberRole::Administrator
                         ? "administrator" : "member") +
           ",\"lastDeliveredSequence\":" +
           std::to_string(member.lastDeliveredSequence) +
           ",\"lastReadSequence\":" +
           std::to_string(member.lastReadSequence) +
           ",\"joinedAt\":" + std::to_string(epochMilliseconds(member.joinedAt)) +
           ",\"mutedUntil\":" +
           (member.mutedUntil
                ? std::to_string(epochMilliseconds(*member.mutedUntil)) : "null") +
           "}";
}

std::string messageJson(const Message &message)
{
    const bool recalled = message.state == MessageState::Recalled;
    return "{\"id\":" + std::to_string(message.id) +
           ",\"conversationId\":" + std::to_string(message.conversationId) +
           ",\"sequence\":" + std::to_string(message.sequence) +
           ",\"senderId\":" + std::to_string(message.senderId) +
           ",\"clientMessageId\":" + quoteJson(message.clientMessageId) +
           ",\"body\":" + quoteJson(recalled ? "" : message.body) +
           ",\"createdAt\":" + std::to_string(epochMilliseconds(message.createdAt)) +
           ",\"state\":" + quoteJson(recalled ? "recalled" : "active") +
           ",\"recalledBy\":" +
           (message.recalledBy ? std::to_string(*message.recalledBy) : "null") +
           ",\"recalledAt\":" +
           (message.recalledAt
                ? std::to_string(epochMilliseconds(*message.recalledAt)) : "null") +
           ",\"modelNodeId\":" +
           (message.modelTrace.nodeId
                ? std::to_string(*message.modelTrace.nodeId) : "null") +
           ",\"modelVersionId\":" +
           (message.modelTrace.versionId
                ? std::to_string(*message.modelTrace.versionId) : "null") +
           ",\"adapterName\":" + quoteJson(message.modelTrace.adapterName) + "}";
}

template<class Range, class Serializer>
std::string arrayJson(const Range &range, Serializer serialize)
{
    std::string result{"["};
    bool first = true;
    for (const auto &value : range)
    {
        if (!first) result.push_back(',');
        first = false;
        result += serialize(value);
    }
    result.push_back(']');
    return result;
}

} // namespace

void Phase11SocialChatHttp::registerRoutes(Router &router)
{
    router.GET("/api/friends", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto friends = runDatabase(
                [this, user = identity.user.id] { return social_.friends(user); });
            reply(context, 200, arrayJson(friends, [this](const auto &contact)
            {
                return contactJson(
                    contact, webSockets_.connectionCount(contact.user.id));
            }));
        });
    });

    router.PATCH("/api/friends/:id", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto contact = unsignedValue(context.param("id"), "contactId");
            const auto fields = bodyObject(context);
            auto remark = required(fields, "remark");
            const auto mutedValue = required(fields, "notificationsMuted");
            if (mutedValue != "true" && mutedValue != "false")
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "notificationsMuted must be true or false");
            const auto updated = runDatabase(
                [this, owner = identity.user.id, contact,
                 remark = std::move(remark), muted = mutedValue == "true"]() mutable
                {
                    return social_.updateContact(owner, contact, std::move(remark),
                                                 muted, currentTime());
                });
            reply(context, 200, contactJson(
                updated, webSockets_.connectionCount(updated.user.id)));
        });
    });

    router.DEL("/api/friends/:id", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto contact = unsignedValue(context.param("id"), "contactId");
            runDatabase([this, user = identity.user.id, contact]
            { social_.removeFriend(user, contact, currentTime()); });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    router.GET("/api/blocks", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto blocked = runDatabase(
                [this, user = identity.user.id] { return social_.blocked(user); });
            reply(context, 200, arrayJson(blocked, userJson));
        });
    });

    router.POST("/api/blocks/:id", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto target = unsignedValue(context.param("id"), "targetId");
            runDatabase([this, user = identity.user.id, target]
            { social_.block(user, target, currentTime()); });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    router.DEL("/api/blocks/:id", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto target = unsignedValue(context.param("id"), "targetId");
            runDatabase([this, user = identity.user.id, target]
            { social_.unblock(user, target, currentTime()); });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    Handler requestFriend = [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto fields = bodyObject(context);
            const auto receiver = unsignedValue(required(fields, "receiverId"),
                                                "receiverId");
            const auto request = runDatabase([this, sender = identity.user.id,
                                              receiver]
            {
                return social_.requestFriendship(sender, receiver, currentTime());
            });
            reply(context, 201, friendRequestJson(request));
        });
    };
    router.POST("/api/friends", requestFriend);
    router.POST("/api/friend-requests", std::move(requestFriend));

    router.GET("/api/friend-requests", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto requests = runDatabase(
                [this, user = identity.user.id] { return social_.pending(user); });
            reply(context, 200, arrayJson(requests, friendRequestJson));
        });
    });

    router.POST("/api/friend-requests/:id", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto requestId = unsignedValue(context.param("id"), "requestId");
            const auto decision = required(bodyObject(context), "accept");
            if (decision != "true" && decision != "false")
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "accept must be true or false");
            const auto request = runDatabase(
                [this, requestId, receiver = identity.user.id,
                 accept = decision == "true"]
                {
                    return social_.decideFriendship(
                        requestId, receiver, accept, currentTime());
                });
            reply(context, 200, friendRequestJson(request));
        });
    });

    router.GET("/api/conversations", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto conversations = runDatabase(
                [this, user = identity.user.id]
                { return chat_.conversations(user); });
            reply(context, 200,
                  arrayJson(conversations, conversationSummaryJson));
        });
    });

    router.POST("/api/conversations", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto fields = bodyObject(context);
            const auto kind = required(fields, "kind");
            Conversation conversation;
            if (kind == "direct")
            {
                const auto peer = unsignedValue(required(fields, "peerId"), "peerId");
                conversation = runDatabase(
                    [this, user = identity.user.id, peer]
                    { return chat_.createDirectConversation(user, peer, currentTime()); });
            }
            else if (kind == "group")
            {
                auto title = required(fields, "title");
                auto members = idList(required(fields, "memberIds"));
                conversation = runDatabase(
                    [this, user = identity.user.id, title = std::move(title),
                     members = std::move(members)]() mutable
                    {
                        return chat_.createGroupConversation(
                            user, std::move(title), std::move(members), currentTime());
                    });
            }
            else
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "kind must be direct or group");
            reply(context, 201, conversationJson(conversation));
        });
    });

    router.GET("/api/conversations/:id/messages", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            const auto after = optionalUnsigned(context, "after", 0);
            const auto limit = optionalUnsigned(context, "limit", 100);
            const auto messages = runDatabase(
                [this, user = identity.user.id, conversation, after, limit]
                { return chat_.history(user, conversation, after, limit); });
            reply(context, 200, arrayJson(messages, messageJson));
        });
    });

    router.GET("/api/conversations/:id/members", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            const auto members = runDatabase(
                [this, user = identity.user.id, conversation]
                { return chat_.members(user, conversation); });
            reply(context, 200, arrayJson(members, memberJson));
        });
    });

    router.POST("/api/conversations/:id/members", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            const auto member = unsignedValue(
                required(bodyObject(context), "memberId"), "memberId");
            runDatabase([this, actor = identity.user.id, conversation, member]
            {
                chat_.addGroupMember(actor, conversation, member, currentTime());
            });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    router.PATCH("/api/conversations/:id", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            const auto fields = bodyObject(context);
            auto title = required(fields, "title");
            auto announcement = required(fields, "announcement");
            const auto updated = runDatabase(
                [this, actor = identity.user.id, conversation,
                 title = std::move(title),
                 announcement = std::move(announcement)]() mutable
                {
                    return chat_.updateGroupProfile(
                        actor, conversation, std::move(title),
                        std::move(announcement), currentTime());
                });
            reply(context, 200, conversationJson(updated));
        });
    });

    router.POST("/api/conversations/:id/members/:memberId/role",
                [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            const auto member = unsignedValue(context.param("memberId"),
                                               "memberId");
            const auto value = required(bodyObject(context), "role");
            const auto role = value == "administrator" ? MemberRole::Administrator
                              : value == "member" ? MemberRole::Member
                              : throw ApplicationError(
                                    ErrorCode::InvalidArgument,
                                    "role must be member or administrator");
            runDatabase([this, actor = identity.user.id, conversation,
                         member, role]
            {
                chat_.setGroupMemberRole(actor, conversation, member, role,
                                         currentTime());
            });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    router.POST("/api/conversations/:id/members/:memberId/mute",
                [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            const auto member = unsignedValue(context.param("memberId"),
                                               "memberId");
            const auto seconds = nonnegativeValue(
                required(bodyObject(context), "seconds"), "seconds");
            if (seconds > 30ULL * 24 * 60 * 60)
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "mute duration cannot exceed 30 days");
            const auto now = currentTime();
            const auto until = seconds == 0
                ? std::optional<TimePoint>{}
                : std::optional<TimePoint>{now + std::chrono::seconds(seconds)};
            runDatabase([this, actor = identity.user.id, conversation,
                         member, until, now]
            {
                chat_.setGroupMemberMute(actor, conversation, member, until, now);
            });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    router.POST("/api/conversations/:id/members/:memberId/remove",
                [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            const auto member = unsignedValue(context.param("memberId"),
                                               "memberId");
            runDatabase([this, actor = identity.user.id, conversation, member]
            {
                chat_.removeGroupMember(actor, conversation, member, currentTime());
            });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    router.POST("/api/conversations/:id/leave", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            runDatabase([this, user = identity.user.id, conversation]
            {
                chat_.leaveGroup(user, conversation, currentTime());
            });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    router.POST("/api/conversations/:id/dissolve", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            runDatabase([this, actor = identity.user.id, conversation]
            {
                chat_.dissolveGroup(actor, conversation, currentTime());
            });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    router.GET("/api/search/messages", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto conversation = optionalUnsigned(
                context, "conversationId", 0);
            const auto query = context.request.querryParams.find("query");
            if (query == context.request.querryParams.end())
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "query is required");
            const auto limit = optionalUnsigned(context, "limit", 50);
            const auto messages = runDatabase(
                [this, user = identity.user.id, conversation,
                 text = query->second, limit]
                { return chat_.search(user, conversation, text, limit); });
            reply(context, 200, arrayJson(messages, messageJson));
        });
    });
}

} // namespace webserver::phase11::transport
