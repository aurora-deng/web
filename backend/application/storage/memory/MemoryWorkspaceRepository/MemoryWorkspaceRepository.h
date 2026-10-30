#pragma once
#include "backend/application/storage/memory/MemoryState/MemoryState.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
namespace webserver::phase11 {
/** Workspace in-memory adapter. Facade holds the shared lock during calls. */
class MemoryWorkspaceRepository final {
public:
    static std::vector<webserver::chat::UserCard> searchUsers(const storage_detail::MemoryState &memory, UserId viewer, std::string_view query, UserId after, std::size_t limit);
    static std::vector<Message> searchVisibleMessages(const storage_detail::MemoryState &memory, UserId viewer, ConversationId conversation, std::string_view query, MessageId before, std::size_t limit);
    static webserver::chat::ConversationPreference preference(const storage_detail::MemoryState &memory, UserId user, ConversationId conversation);
    static webserver::chat::PreferenceWrite writePreference(storage_detail::MemoryState &memory, UserId user, ConversationId conversation, bool muted);
};
}
