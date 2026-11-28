#pragma once
#include "backend/application/modules/chat/domain/ConversationTypes/ConversationTypes.h"
#include "backend/application/modules/chat/domain/MessageTypes/MessageTypes.h"
#include "backend/application/modules/chat/domain/ConversationParticipant/ConversationParticipant.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webserver::phase11 {

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

} // namespace webserver::phase11
