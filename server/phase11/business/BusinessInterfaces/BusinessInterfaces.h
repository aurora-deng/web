#pragma once

#include "server/phase11/domain/Domain/Domain.h"

#include <chrono>
#include <cstddef>
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

struct TrainingSubmission
{
    UserId ownerUserId = 0;
    MessageId sourceMessageId = 0;
    std::string sanitizedPrompt;
    std::string sanitizedResponse;
    std::string consentVersion;
    bool consentGranted = false;
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

/**
 * Citation 的业务视图：ID 用于审计，标题、来源和片段用于前端解释回答依据。
 * 传输层只负责序列化，不再自行跨 Repository 拼装资料。
 */
struct KnowledgeCitationView
{
    RagCitation citation;
    std::string documentTitle;
    std::string sourceName;
    std::uint64_t chunkOrdinal = 0;
    std::string content;
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
    [[nodiscard]] virtual std::vector<FriendContact> friends(
        UserId user) const = 0;
    [[nodiscard]] virtual std::vector<FriendRequest> pending(
        UserId receiver) const = 0;
    virtual FriendContact updateContact(UserId owner, UserId contact,
                                        std::string remark,
                                        bool notificationsMuted,
                                        TimePoint now) = 0;
    virtual void removeFriend(UserId user, UserId contact, TimePoint now) = 0;
    virtual void block(UserId user, UserId target, TimePoint now) = 0;
    virtual void unblock(UserId user, UserId target, TimePoint now) = 0;
    [[nodiscard]] virtual std::vector<User> blocked(UserId user) const = 0;
    virtual void recordPresence(UserId user, std::size_t connections,
                                TimePoint now) = 0;
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
    virtual Conversation updateGroupProfile(UserId actor,
                                            ConversationId conversation,
                                            std::string title,
                                            std::string announcement,
                                            TimePoint now) = 0;
    virtual void setGroupMemberRole(UserId actor, ConversationId conversation,
                                    UserId member, MemberRole role,
                                    TimePoint now) = 0;
    virtual void setGroupMemberMute(
        UserId actor, ConversationId conversation, UserId member,
        std::optional<TimePoint> mutedUntil, TimePoint now) = 0;
    virtual void removeGroupMember(UserId actor, ConversationId conversation,
                                   UserId member, TimePoint now) = 0;
    virtual void leaveGroup(UserId user, ConversationId conversation,
                            TimePoint now) = 0;
    virtual void dissolveGroup(UserId actor, ConversationId conversation,
                               TimePoint now) = 0;
    virtual AppendMessageResult sendText(UserId sender,
                                         ConversationId conversation,
                                         std::string clientMessageId,
                                         std::string body, TimePoint now,
                                         MessageModelTrace trace = {}) = 0;
    virtual AppendMessageResult sendFile(UserId sender,
                                         ConversationId conversation,
                                         std::string clientMessageId,
                                         std::string caption, FileId file,
                                         TimePoint now) = 0;
    virtual RecallMessageResult recallMessage(UserId actor, MessageId message,
                                              TimePoint now) = 0;
    virtual void acknowledgeDelivery(UserId user, ConversationId conversation,
                                     std::uint64_t sequence, TimePoint now) = 0;
    virtual void markRead(UserId user, ConversationId conversation,
                          std::uint64_t sequence, TimePoint now) = 0;
    [[nodiscard]] virtual std::vector<ConversationSummary> conversations(
        UserId user) const = 0;
    [[nodiscard]] virtual std::vector<ConversationParticipant> members(
        UserId user, ConversationId conversation) const = 0;
    [[nodiscard]] virtual std::vector<Message> history(
        UserId user, ConversationId conversation,
        std::uint64_t afterSequence, std::size_t limit) const = 0;
    [[nodiscard]] virtual std::vector<Message> historyBefore(
        UserId user, ConversationId conversation,
        std::uint64_t beforeSequence, std::size_t limit) const = 0;
    [[nodiscard]] virtual std::optional<Message> ownMessage(
        UserId user, ConversationId conversation,
        std::string_view clientMessageId) const = 0;
    [[nodiscard]] virtual std::vector<Message> search(
        UserId user, ConversationId conversation, std::string_view query,
        std::size_t limit) const = 0;
};

class IFileBusiness
{
public:
    virtual ~IFileBusiness() = default;
    virtual FileObject createUpload(UserId owner, std::string originalName,
                                    std::string mediaType,
                                    std::uint64_t expectedSize,
                                    TimePoint now) = 0;
    virtual FileObject appendChunk(UserId owner, FileId file,
                                   std::uint64_t offset,
                                   std::string_view bytes) = 0;
    virtual FileObject completeUpload(UserId owner, FileId file,
                                      TimePoint now) = 0;
    [[nodiscard]] virtual FileDownload download(UserId requester,
                                                FileId file) const = 0;
    virtual User useAsAvatar(UserId owner, FileId file) = 0;
};

class IKnowledgeBusiness
{
public:
    virtual ~IKnowledgeBusiness() = default;

    virtual KnowledgeDocument registerKnowledgeDocument(
        UserId owner, std::optional<ConversationId> conversation,
        std::string title, std::string sourceName,
        std::string mediaType, std::string content, TimePoint now) = 0;
    [[nodiscard]] virtual std::vector<KnowledgeDocument> knowledgeDocuments(
        UserId requester,
        std::optional<ConversationId> conversation) const = 0;
    [[nodiscard]] virtual std::vector<KnowledgeCitationView>
    generationCitations(UserId requester,
                        std::string_view generationId) const = 0;
};

class ITrainingBusiness
{
public:
    virtual ~ITrainingBusiness() = default;

    virtual TrainingCandidate submitTrainingCandidate(
        TrainingSubmission submission, TimePoint now) = 0;
    [[nodiscard]] virtual std::vector<TrainingCandidate>
    trainingCandidates(UserId owner) const = 0;
    virtual TrainingCandidate revokeTrainingCandidate(
        TrainingCandidateId candidate, UserId owner) = 0;
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
