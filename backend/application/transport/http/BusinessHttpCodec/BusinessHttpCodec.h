#pragma once
#include "backend/application/business/ApplicationError/ApplicationError.h"
#include "backend/application/domain/Domain/Domain.h"
#include "backend/application/transport/FlatJson/FlatJson.h"
#include "backend/application/transport/serialization/MessageJson/MessageJson.h"
#include "backend/server/http/RequestContext/RequestContext.h"
#include <charconv>
#include <chrono>
#include <stdexcept>
#include <string_view>
namespace webserver::phase11::transport::http_codec {
inline TimePoint currentTime()
{
    return std::chrono::system_clock::now();
}

inline std::string statusText(int status)
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

inline std::pair<int, std::string_view> mapError(ErrorCode code)
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

inline void reply(RequestContext &context, int status, std::string body)
{
    context.response->status = status;
    context.response->statusText = statusText(status);
    context.response->json(body);
    context.response->setHeader("Cache-Control", "no-store");
}

inline void error(RequestContext &context, int status,
           std::string_view code, std::string_view message)
{
    reply(context, status,
          "{\"code\":" + quoteJson(code) +
              ",\"message\":" + quoteJson(message) + "}");
}

template<class Operation>
inline bool guarded(RequestContext &context, Operation &&operation)
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

inline FlatStringObject bodyObject(const RequestContext &context)
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

inline std::string required(const FlatStringObject &fields, std::string_view name)
{
    const auto found = fields.find(std::string(name));
    if (found == fields.end())
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "missing field: " + std::string(name));
    return found->second;
}

inline std::uint64_t unsignedValue(std::string_view value, std::string_view field)
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

inline std::uint64_t nonnegativeValue(std::string_view value, std::string_view field)
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

inline std::uint64_t optionalUnsigned(const RequestContext &context,
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

inline std::vector<UserId> idList(std::string_view input)
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

inline std::string userJson(const User &user)
{
    return "{\"id\":" + std::to_string(user.id) +
           ",\"username\":" + quoteJson(user.username) +
           ",\"displayName\":" + quoteJson(user.displayName) +
           ",\"biography\":" + quoteJson(user.biography) +
           ",\"aiAccount\":" + (user.aiAccount ? "true" : "false") +
           ",\"avatarFileId\":" +
           (user.avatarFileId ? std::to_string(*user.avatarFileId) : "null") +
           ",\"avatarUrl\":" +
           (user.avatarFileId
                ? quoteJson("/api/files/" + std::to_string(*user.avatarFileId))
                : "null") + "}";
}

inline std::string friendRequestJson(const FriendRequest &request)
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

inline std::string contactJson(const FriendContact &contact,
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

inline std::string conversationJson(const Conversation &conversation)
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

inline std::string conversationSummaryJson(const ConversationSummary &summary)
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
           ",\"latestSequence\":" + quoteJson(std::to_string(conversation.nextSequence - 1)) +
           ",\"readSequence\":" + quoteJson(std::to_string(summary.lastReadSequence)) +
           ",\"unreadCount\":" + std::to_string(summary.unreadCount) +
           ",\"displayTitle\":" + quoteJson(summary.displayTitle) +
           ",\"peerId\":" + (summary.peerUserId ? quoteJson(std::to_string(*summary.peerUserId)) : "null") +
           ",\"avatarUrl\":" + (summary.avatarFileId ? quoteJson("/api/files/" + std::to_string(*summary.avatarFileId)) : "null") +
           ",\"lastMessagePreview\":" + quoteJson(summary.lastMessagePreview) +
           ",\"lastMessageAt\":" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(summary.lastMessageAt.time_since_epoch()).count()) +
           ",\"notificationsMuted\":" + (summary.notificationsMuted ? "true" : "false") +
           ",\"preferencesVersion\":" + quoteJson(std::to_string(summary.preferencesRevision)) +
           ",\"containsAi\":" + (summary.containsAi ? "true" : "false") + "}";
}

inline std::int64_t epochMilliseconds(TimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               value.time_since_epoch()).count();
}

inline std::string memberJson(const ConversationParticipant &participant)
{
    const auto &member = participant.membership;
    const auto &user = participant.user;
    return "{\"userId\":" + std::to_string(member.userId) +
           ",\"username\":" + quoteJson(user.username) +
           ",\"displayName\":" + quoteJson(user.displayName) +
           ",\"aiAccount\":" + (user.aiAccount ? "true" : "false") +
           ",\"avatarUrl\":" +
           (user.avatarFileId
                ? quoteJson("/api/files/" + std::to_string(*user.avatarFileId))
                : "null") +
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

inline std::string messageJson(const Message &message)
{
    return serializeMessageJson(message);
}

template<class Range, class Serializer>
inline std::string arrayJson(const Range &range, Serializer serialize)
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


}
