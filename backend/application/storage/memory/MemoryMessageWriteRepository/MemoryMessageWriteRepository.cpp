#include "backend/application/storage/memory/MemoryMessageWriteRepository/MemoryMessageWriteRepository.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>
namespace webserver::phase11 {
using storage_detail::orderedUsers;
using storage_detail::UserPair;
using storage_detail::MemberKey;
using storage_detail::IdempotencyKey;
AppendMessageResult MemoryMessageWriteRepository::appendMessage(storage_detail::MemoryState &memory, Message message)
{
    const IdempotencyKey key{
        message.conversationId, message.senderId, message.clientMessageId};
    if (message.clientMessageId.empty())
        throw std::invalid_argument("client message id is required");
    if (const auto duplicate = memory.idempotency.find(key);
        duplicate != memory.idempotency.end())
        return {MemoryMessageWriteRepository::hydrateMessageLocked(memory, memory.messages.at(duplicate->second)), false};

    const auto conversation = memory.conversations.find(message.conversationId);
    if (conversation == memory.conversations.end())
        throw std::invalid_argument("conversation does not exist");
    if (conversation->second.state != ConversationState::Active)
        throw std::invalid_argument("conversation is not active");
    message.id = memory.nextMessageId++;
    message.sequence = conversation->second.nextSequence++;
    memory.messages.emplace(message.id, message);
    memory.messageIds[message.conversationId].push_back(message.id);
    memory.idempotency.emplace(key, message.id);
    return {message, true};
}

Message MemoryMessageWriteRepository::hydrateMessageLocked(const storage_detail::MemoryState &memory, Message message)
{
    message.attachments.clear();
    const auto linked = memory.attachments.find(message.id);
    if (linked == memory.attachments.end()) return message;
    for (const auto fileId : linked->second)
    {
        const auto file = memory.files.find(fileId);
        if (file == memory.files.end() || file->second.state != FileState::Ready)
            continue;
        message.attachments.push_back(
            {fileId, file->second.originalName, file->second.mediaType,
             file->second.expectedSize, file->second.sha256});
    }
    return message;
}

std::vector<Message> MemoryMessageWriteRepository::messagesAfter(const storage_detail::MemoryState &memory, ConversationId conversation, std::uint64_t sequence, std::size_t limit)
{
    std::vector<Message> result;
    const auto found = memory.messageIds.find(conversation);
    if (found == memory.messageIds.end())
        return result;
    for (const auto id : found->second)
    {
        const auto &message = memory.messages.at(id);
        if (message.sequence > sequence)
        {
            result.push_back(MemoryMessageWriteRepository::hydrateMessageLocked(memory, message));
            if (result.size() == limit)
                break;
        }
    }
    return result;
}

std::vector<Message> MemoryMessageWriteRepository::messagesBefore(const storage_detail::MemoryState &memory, ConversationId conversation, std::uint64_t before, std::size_t limit)
{
    std::vector<Message> result;
    const auto found = memory.messageIds.find(conversation);
    if (found == memory.messageIds.end() || !limit) return result;
    // 从最新记录反向取一页，再恢复升序；游标不是 OFFSET，因此新消息插入不挤动旧页。
    for (auto it = found->second.rbegin(); it != found->second.rend(); ++it)
    {
        const auto &message = memory.messages.at(*it);
        if (before && message.sequence >= before) continue;
        result.push_back(MemoryMessageWriteRepository::hydrateMessageLocked(memory, message));
        if (result.size() == limit) break;
    }
    std::reverse(result.begin(), result.end());
    return result;
}

std::optional<Message> MemoryMessageWriteRepository::findOwnMessage(const storage_detail::MemoryState &memory, ConversationId conversation, UserId sender, std::string_view clientMessageId)
{
    const auto found = memory.idempotency.find(
        IdempotencyKey{conversation, sender, std::string(clientMessageId)});
    return found == memory.idempotency.end() ? std::nullopt
        : std::optional<Message>{MemoryMessageWriteRepository::hydrateMessageLocked(memory, memory.messages.at(found->second))};
}

std::vector<Message> MemoryMessageWriteRepository::searchMessages(const storage_detail::MemoryState &memory, ConversationId conversation, std::string_view query, std::size_t limit)
{
    std::vector<Message> result;
    const auto found = memory.messageIds.find(conversation);
    if (found == memory.messageIds.end() || query.empty())
        return result;
    for (const auto id : found->second)
    {
        const auto &message = memory.messages.at(id);
        if (message.state == MessageState::Active &&
            message.body.find(query) != std::string::npos)
        {
            result.push_back(MemoryMessageWriteRepository::hydrateMessageLocked(memory, message));
            if (result.size() == limit)
                break;
        }
    }
    return result;
}

std::optional<Message> MemoryMessageWriteRepository::findMessage(const storage_detail::MemoryState &memory, MessageId id)
{
    const auto found = memory.messages.find(id);
    return found == memory.messages.end()
               ? std::nullopt
               : std::optional<Message>{MemoryMessageWriteRepository::hydrateMessageLocked(memory, found->second)};
}

RecallMessageResult MemoryMessageWriteRepository::recallMessage(storage_detail::MemoryState &memory, MessageId id, UserId actor, TimePoint now)
{
    const auto found = memory.messages.find(id);
    if (found == memory.messages.end())
        return {};
    if (found->second.state == MessageState::Recalled)
        return {MemoryMessageWriteRepository::hydrateMessageLocked(memory, found->second), false};
    found->second.state = MessageState::Recalled;
    found->second.recalledBy = actor;
    found->second.recalledAt = now;
    return {MemoryMessageWriteRepository::hydrateMessageLocked(memory, found->second), true};
}

}
