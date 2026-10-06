#pragma once

#include "server/phase11/domain/Domain/Domain.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webserver::phase11::transport
{

/**
 * 这里只定义“线上包裹”的字段，不负责 JSON 解析，也不包含 HttpRequest、WebSocketFrame
 * 或数据库类型。未来 HTTP、WebSocket 和 SSE 适配器都把各自协议转换成这些 DTO，再调用
 * Application Service；这样业务规则不会散落在三种协议处理器里。
 */
struct AuthContext
{
    UserId userId = 0;
    std::string sessionId;
};

/** 可进入 JSON 的公开用户视图，刻意不包含 User::passwordHash。 */
struct PublicUser
{
    UserId id = 0;
    std::string username;
    std::string displayName;
    std::string biography;
    bool aiAccount = false;
};

/** Session Token 只写入 HttpOnly Cookie，响应 JSON 仅返回公开用户和 CSRF Token。 */
struct LoginResponse
{
    PublicUser user;
    std::string csrfToken;
};

enum class SameSitePolicy
{
    Strict,
    Lax,
    None
};

struct SessionCookiePolicy
{
    std::string name = "phase11_session";
    std::string path = "/";
    bool secure = true;
    bool httpOnly = true;
    SameSitePolicy sameSite = SameSitePolicy::Strict;
};

struct RegisterRequest
{
    std::string username;
    std::string password;
    std::string displayName;
};

struct LoginRequest
{
    std::string username;
    std::string password;
};

struct ProfilePatchRequest
{
    std::string displayName;
    std::string biography;
};

struct FriendRequestCreate
{
    UserId receiverId = 0;
};

struct FriendRequestDecision
{
    FriendRequestId requestId = 0;
    bool accept = false;
};

struct ConversationCreate
{
    ConversationKind kind = ConversationKind::Direct;
    std::string title;
    std::vector<UserId> memberIds;
};

struct ChatSend
{
    ConversationId conversationId = 0;
    std::string clientMessageId;
    std::string body;
};

struct ChatAck
{
    ConversationId conversationId = 0;
    std::uint64_t sequence = 0;
};

struct PresenceSubscription
{
    std::vector<UserId> userIds;
};

struct PresenceUpdate
{
    UserId userId = 0;
    bool online = false;
};

struct ErrorResponse
{
    std::string code;
    std::string message;
    std::string requestId;
};

struct SseEvent
{
    std::string id;
    std::string type;
    std::string jsonData;
    std::optional<std::uint32_t> retryMilliseconds;
};

inline constexpr std::string_view kChatSend = "chat.send";
inline constexpr std::string_view kChatAck = "chat.ack";
inline constexpr std::string_view kChatRead = "chat.read";
inline constexpr std::string_view kPresenceSubscribe = "presence.subscribe";
inline constexpr std::string_view kPresenceUpdate = "presence.update";
inline constexpr std::string_view kAiGenerate = "ai.generate";
inline constexpr std::string_view kAiCancel = "ai.cancel";

inline constexpr std::string_view kFriendRequest = "friend.request";
inline constexpr std::string_view kConversationInvited = "conversation.invited";
inline constexpr std::string_view kMessageNotification = "message.notification";
inline constexpr std::string_view kAiToken = "ai.token";
inline constexpr std::string_view kAiCompleted = "ai.completed";
inline constexpr std::string_view kModelStatus = "model.status";

} // namespace webserver::phase11::transport
