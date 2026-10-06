#pragma once

#include "server/phase11/domain/Domain/Domain.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webserver::phase11
{

/**
 * 浏览器协议适配器与业务实现之间的稳定边界。
 *
 * HTTP、WebSocket 和 SSE 像不同窗口；这些接口是窗口后面的统一办事规则。
 * 协议层只负责认证信息、JSON/帧和状态码转换，业务层负责账号、好友、会话、
 * 消息持久化、权限与 AI 用例，因此更换前端或协议不会复制业务规则。
 */

struct LoginSession
{
    User user;
    std::string sessionToken;
    std::string csrfToken;
    TimePoint expiresAt{};
};

struct RestoredSession
{
    User user;
    std::string csrfToken;
    TimePoint expiresAt{};
};

struct AiChatRequest
{
    UserId requester = 0;
    UserId aiUser = 0;
    ConversationId conversation = 0;
    std::string requestId;
    std::string prompt;
    std::size_t maxOutputTokens = 512;
};

/**
 * 会话成员视图：把“成员关系”和“用户公开资料”一次交给传输层。
 * 前端因此可以识别 AI 账号、显示名称并自动选择 AI 目标，不必要求用户死记 ID。
 */
struct ConversationParticipant
{
    ConversationMember membership;
    User user;
};

class IAuthBusiness
{
public:
    virtual ~IAuthBusiness() = default;

    virtual User registerUser(std::string username, std::string password,
                              std::string displayName, TimePoint now) = 0;
    virtual LoginSession login(std::string_view username,
                               std::string_view password, TimePoint now,
                               std::string_view origin = {}) = 0;
    [[nodiscard]] virtual std::optional<RestoredSession> restoreSession(
        std::string_view sessionToken, TimePoint now) const = 0;
    [[nodiscard]] virtual std::optional<User> authenticate(
        std::string_view sessionToken, TimePoint now) const = 0;
    [[nodiscard]] virtual bool verifyCsrf(std::string_view sessionToken,
                                          std::string_view csrfToken,
                                          TimePoint now) const = 0;
    virtual void logout(std::string_view sessionToken, TimePoint now) = 0;
    virtual User updateProfile(UserId user, std::string displayName,
                               std::string biography) = 0;
};

class ISocialBusiness
{
public:
    virtual ~ISocialBusiness() = default;

    virtual FriendRequest requestFriendship(UserId sender, UserId receiver,
                                            TimePoint now) = 0;
    virtual FriendRequest decideFriendship(FriendRequestId request,
                                           UserId receiver, bool accept,
                                           TimePoint now) = 0;
    [[nodiscard]] virtual std::vector<User> friends(UserId user) const = 0;
    [[nodiscard]] virtual std::vector<FriendRequest> pending(
        UserId receiver) const = 0;
};

class IChatBusiness
{
public:
    virtual ~IChatBusiness() = default;

    virtual Conversation createDirectConversation(UserId requester, UserId peer,
                                                   TimePoint now) = 0;
    virtual Conversation createGroupConversation(UserId owner, std::string title,
                                                  std::vector<UserId> members,
                                                  TimePoint now) = 0;
    virtual void addGroupMember(UserId actor, ConversationId conversation,
                                UserId member, TimePoint now) = 0;
    virtual AppendMessageResult sendText(UserId sender,
                                         ConversationId conversation,
                                         std::string clientMessageId,
                                         std::string body, TimePoint now,
                                         MessageModelTrace trace = {}) = 0;
    virtual void acknowledgeDelivery(UserId user, ConversationId conversation,
                                     std::uint64_t sequence) = 0;
    virtual void markRead(UserId user, ConversationId conversation,
                          std::uint64_t sequence) = 0;
    [[nodiscard]] virtual std::vector<Conversation> conversations(
        UserId user) const = 0;
    [[nodiscard]] virtual std::vector<ConversationParticipant> members(
        UserId user, ConversationId conversation) const = 0;
    [[nodiscard]] virtual std::vector<Message> history(
        UserId user, ConversationId conversation,
        std::uint64_t afterSequence, std::size_t limit) const = 0;
    [[nodiscard]] virtual std::vector<Message> search(
        UserId user, ConversationId conversation, std::string_view query,
        std::size_t limit) const = 0;
};

class IAiChatBusiness
{
public:
    virtual ~IAiChatBusiness() = default;
    virtual void generate(AiChatRequest request) = 0;
    [[nodiscard]] virtual bool cancel(UserId requester,
                                      const std::string &generationId) noexcept = 0;
};

} // namespace webserver::phase11
