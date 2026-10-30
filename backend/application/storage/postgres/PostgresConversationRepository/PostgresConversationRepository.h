#pragma once
#include "backend/application/domain/ConversationTypes/ConversationTypes.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
#include "backend/application/storage/postgres/PostgresOutboxWriter/PostgresOutboxWriter.h"
namespace webserver::phase11 {
/** Conversation SQL adapter. Uses the caller's connection; never borrows another lease. */
class PostgresConversationRepository final {
public:
    static Conversation createConversation(PGconn *connection, bool ownsTransaction, Conversation conversation, std::vector<ConversationMember> members);
    static std::optional<Conversation> findConversation(PGconn *connection, bool transactionActive, ConversationId id);
    static std::optional<ConversationMember> findMember(PGconn *connection, ConversationId conversation, UserId user);
    static std::optional<Conversation> findDirectConversation(PGconn *connection, UserId first, UserId second);
    static std::vector<Conversation> listConversations(PGconn *connection, UserId user);
    static std::vector<ConversationSummary> listConversationSummaries(PGconn *connection, UserId user);
    static std::vector<ConversationMember> listMembers(PGconn *connection, ConversationId conversation);
    static bool addMember(PGconn *connection, ConversationMember member);
    static bool updateConversationProfile(PGconn *connection, ConversationId conversation, std::string title, std::string announcement);
    static bool dissolveConversation(PGconn *connection, ConversationId conversation, UserId actor, TimePoint now);
    static bool updateMemberRole(PGconn *connection, ConversationId conversation, UserId user, MemberRole role);
    static bool updateMemberMute(PGconn *connection, ConversationId conversation, UserId user, std::optional<TimePoint> mutedUntil);
    static bool removeMember(PGconn *connection, ConversationId conversation, UserId user);
    static bool updateDelivered(PGconn *connection, ConversationId conversation, UserId user, std::uint64_t sequence);
    static bool updateRead(PGconn *connection, ConversationId conversation, UserId user, std::uint64_t sequence);
};
}
