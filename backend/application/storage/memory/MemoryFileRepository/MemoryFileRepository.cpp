#include "backend/application/storage/memory/MemoryFileRepository/MemoryFileRepository.h"
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
FileObject MemoryFileRepository::createFile(storage_detail::MemoryState &memory, FileObject file)
{
    if (!memory.users.contains(file.ownerId) || file.expectedSize == 0)
        throw std::invalid_argument("invalid file metadata");
    file.id = memory.nextFileId++;
    memory.files.emplace(file.id, file);
    return file;
}

std::optional<FileObject> MemoryFileRepository::findFile(const storage_detail::MemoryState &memory, FileId id)
{
    const auto found = memory.files.find(id);
    return found == memory.files.end() ? std::nullopt
                                      : std::optional<FileObject>{found->second};
}

bool MemoryFileRepository::updateFileProgress(storage_detail::MemoryState &memory, FileId id, UserId owner, std::uint64_t expectedOffset, std::uint64_t uploadedSize)
{
    const auto found = memory.files.find(id);
    if (found == memory.files.end() || found->second.ownerId != owner ||
        found->second.state != FileState::Pending ||
        found->second.uploadedSize != expectedOffset ||
        uploadedSize < expectedOffset || uploadedSize > found->second.expectedSize)
        return false;
    found->second.uploadedSize = uploadedSize;
    return true;
}

bool MemoryFileRepository::completeFile(storage_detail::MemoryState &memory, FileId id, UserId owner, std::string sha256, TimePoint now)
{
    const auto found = memory.files.find(id);
    if (found == memory.files.end() || found->second.ownerId != owner ||
        found->second.state != FileState::Pending ||
        found->second.uploadedSize != found->second.expectedSize ||
        sha256.size() != 64)
        return false;
    found->second.sha256 = std::move(sha256);
    found->second.state = FileState::Ready;
    found->second.readyAt = now;
    return true;
}

bool MemoryFileRepository::failFile(storage_detail::MemoryState &memory, FileId id, UserId owner)
{
    const auto found = memory.files.find(id);
    if (found == memory.files.end() || found->second.ownerId != owner ||
        found->second.state != FileState::Pending)
        return false;
    found->second.state = FileState::Failed;
    return true;
}

bool MemoryFileRepository::attachFile(storage_detail::MemoryState &memory, MessageId message, FileId file)
{
    const auto foundMessage = memory.messages.find(message);
    const auto foundFile = memory.files.find(file);
    if (foundMessage == memory.messages.end() || foundFile == memory.files.end() ||
        foundFile->second.state != FileState::Ready ||
        foundMessage->second.senderId != foundFile->second.ownerId)
        return false;
    auto &linked = memory.attachments[message];
    if (!linked.empty()) return false;
    linked.push_back(file);
    return true;
}

std::vector<MessageAttachment> MemoryFileRepository::attachmentsForMessage(const storage_detail::MemoryState &memory, MessageId message)
{
    Message stub;
    stub.id = message;
    return MemoryMessageWriteRepository::hydrateMessageLocked(memory, std::move(stub)).attachments;
}

bool MemoryFileRepository::canAccessFile(const storage_detail::MemoryState &memory, UserId user, FileId file)
{
    const auto foundFile = memory.files.find(file);
    if (foundFile == memory.files.end() ||
        foundFile->second.state != FileState::Ready)
        return false;
    if (foundFile->second.ownerId == user) return true;
    for (const auto &[_, profile] : memory.users)
        if (profile.avatarFileId == file) return true;
    for (const auto &[messageId, linked] : memory.attachments)
    {
        if (std::find(linked.begin(), linked.end(), file) == linked.end()) continue;
        const auto message = memory.messages.find(messageId);
        if (message != memory.messages.end() &&
            memory.members.contains({message->second.conversationId, user}))
            return true;
    }
    return false;
}

}
