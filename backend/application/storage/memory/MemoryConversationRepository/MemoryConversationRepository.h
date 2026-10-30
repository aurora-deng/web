#pragma once
#include "backend/application/storage/memory/MemoryState/MemoryState.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
namespace webserver::phase11 {
/** Conversation in-memory adapter. Facade holds the shared lock during calls. */
class MemoryConversationRepository final {
public:
    static Conversation createConversation(storage_detail::MemoryState &memory, Conversation conversation, std::vector<ConversationMember> members);
    static std::optional<Conversation> findConversation(const storage_detail::MemoryState &memory, ConversationId id);
    static std::optional<ConversationMember> findMember(const storage_detail::MemoryState &memory, ConversationId conversation, UserId user);
    static std::optional<Conversation> findDirectConversation(const storage_detail::MemoryState &memory, UserId first, UserId second);
    static std::vector<Conversation> listConversations(const storage_detail::MemoryState &memory, UserId user);
    static std::vector<ConversationSummary> listConversationSummaries(const storage_detail::MemoryState &memory, UserId user);
    static std::vector<ConversationMember> listMembers(const storage_detail::MemoryState &memory, ConversationId conversation);
    static bool addMember(storage_detail::MemoryState &memory, ConversationMember member);
    static bool updateConversationProfile(storage_detail::MemoryState &memory, ConversationId conversation, std::string title, std::string announcement);
    static bool dissolveConversation(storage_detail::MemoryState &memory, ConversationId conversation, UserId actor, TimePoint now);
    static bool updateMemberRole(storage_detail::MemoryState &memory, ConversationId conversation, UserId user, MemberRole role);
    static bool updateMemberMute(storage_detail::MemoryState &memory, ConversationId conversation, UserId user, std::optional<TimePoint> mutedUntil);
    static bool removeMember(storage_detail::MemoryState &memory, ConversationId conversation, UserId user);
    static bool updateDelivered(storage_detail::MemoryState &memory, ConversationId conversation, UserId user, std::uint64_t sequence);
    static bool updateRead(storage_detail::MemoryState &memory, ConversationId conversation, UserId user, std::uint64_t sequence);
};
}
