#pragma once
#include "backend/application/storage/memory/MemoryState/MemoryState.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
namespace webserver::phase11 {
/** MessageWrite in-memory adapter. Facade holds the shared lock during calls. */
class MemoryMessageWriteRepository final {
public:
    static AppendMessageResult appendMessage(storage_detail::MemoryState &memory, Message message);
    static Message hydrateMessageLocked(const storage_detail::MemoryState &memory, Message message);
    static std::vector<Message> messagesAfter(const storage_detail::MemoryState &memory, ConversationId conversation, std::uint64_t sequence, std::size_t limit);
    static std::vector<Message> messagesBefore(const storage_detail::MemoryState &memory, ConversationId conversation, std::uint64_t before, std::size_t limit);
    static std::optional<Message> findOwnMessage(const storage_detail::MemoryState &memory, ConversationId conversation, UserId sender, std::string_view clientMessageId);
    static std::vector<Message> searchMessages(const storage_detail::MemoryState &memory, ConversationId conversation, std::string_view query, std::size_t limit);
    static std::optional<Message> findMessage(const storage_detail::MemoryState &memory, MessageId id);
    static RecallMessageResult recallMessage(storage_detail::MemoryState &memory, MessageId id, UserId actor, TimePoint now);
};
}
