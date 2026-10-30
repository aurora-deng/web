#pragma once
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include "backend/application/domain/ConversationTypes/ConversationTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace webserver::phase11 {
class IConversationRepository
{
public:
    virtual ~IConversationRepository() = default;
    virtual Conversation createConversation(
        Conversation conversation,
        std::vector<ConversationMember> members) = 0;
    [[nodiscard]] virtual std::optional<Conversation> findConversation(
        ConversationId id) const = 0;
    [[nodiscard]] virtual std::optional<ConversationMember> findMember(
        ConversationId conversation, UserId user) const = 0;
    [[nodiscard]] virtual std::optional<Conversation> findDirectConversation(
        UserId first, UserId second) const = 0;
    [[nodiscard]] virtual std::vector<Conversation> listConversations(
        UserId user) const = 0;
    [[nodiscard]] virtual std::vector<ConversationSummary>
    listConversationSummaries(UserId user) const = 0;
    [[nodiscard]] virtual std::vector<ConversationMember> listMembers(
        ConversationId conversation) const = 0;
    virtual bool addMember(ConversationMember member) = 0;
    virtual bool updateConversationProfile(ConversationId conversation,
                                           std::string title,
                                           std::string announcement) = 0;
    virtual bool dissolveConversation(ConversationId conversation,
                                      UserId actor, TimePoint now) = 0;
    virtual bool updateMemberRole(ConversationId conversation, UserId user,
                                  MemberRole role) = 0;
    virtual bool updateMemberMute(ConversationId conversation, UserId user,
                                  std::optional<TimePoint> mutedUntil) = 0;
    virtual bool removeMember(ConversationId conversation, UserId user) = 0;
    virtual bool updateDelivered(ConversationId conversation, UserId user,
                                 std::uint64_t sequence) = 0;
    virtual bool updateRead(ConversationId conversation, UserId user,
                            std::uint64_t sequence) = 0;
};
}
