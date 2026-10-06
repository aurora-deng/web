#pragma once

#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/business/BusinessInterfaces/BusinessInterfaces.h"
#include "server/phase11/ports/Ports/Ports.h"

namespace webserver::phase11
{

class ChatService final : public IChatBusiness
{
public:
    ChatService(IDatabase &database,
                IUserRepository &users,
                ISocialRepository &social,
                IConversationRepository &conversations,
                IMessageRepository &messages,
                IOutboxRepository &outbox)
        : database_(database), users_(users), social_(social),
          conversations_(conversations), messages_(messages), outbox_(outbox)
    {
    }

    Conversation createDirectConversation(UserId requester, UserId peer,
                                          TimePoint now) override;
    Conversation createGroupConversation(UserId owner, std::string title,
                                         std::vector<UserId> members,
                                         TimePoint now) override;
    void addGroupMember(UserId actor, ConversationId conversation,
                        UserId member, TimePoint now) override;
    AppendMessageResult sendText(UserId sender, ConversationId conversation,
                                 std::string clientMessageId,
                                 std::string body, TimePoint now,
                                 MessageModelTrace trace = {}) override;
    void acknowledgeDelivery(UserId user, ConversationId conversation,
                             std::uint64_t sequence) override;
    void markRead(UserId user, ConversationId conversation,
                  std::uint64_t sequence) override;
    std::vector<Conversation> conversations(UserId user) const override;
    std::vector<ConversationParticipant> members(
        UserId user, ConversationId conversation) const override;
    std::vector<Message> history(UserId user, ConversationId conversation,
                                 std::uint64_t afterSequence,
                                 std::size_t limit) const override;
    std::vector<Message> search(UserId user, ConversationId conversation,
                                std::string_view query,
                                std::size_t limit) const override;

private:
    ConversationMember requireMember(UserId user,
                                     ConversationId conversation) const;
    Conversation requireConversation(ConversationId conversation) const;

    IDatabase &database_;
    IUserRepository &users_;
    ISocialRepository &social_;
    IConversationRepository &conversations_;
    IMessageRepository &messages_;
    IOutboxRepository &outbox_;
};

} // namespace webserver::phase11
