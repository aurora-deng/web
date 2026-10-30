#pragma once
#include "backend/application/storage/memory/MemoryState/MemoryState.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
namespace webserver::phase11 {
/** File in-memory adapter. Facade holds the shared lock during calls. */
class MemoryFileRepository final {
public:
    static FileObject createFile(storage_detail::MemoryState &memory, FileObject file);
    static std::optional<FileObject> findFile(const storage_detail::MemoryState &memory, FileId id);
    static bool updateFileProgress(storage_detail::MemoryState &memory, FileId id, UserId owner, std::uint64_t expectedOffset, std::uint64_t uploadedSize);
    static bool completeFile(storage_detail::MemoryState &memory, FileId id, UserId owner, std::string sha256, TimePoint now);
    static bool failFile(storage_detail::MemoryState &memory, FileId id, UserId owner);
    static bool attachFile(storage_detail::MemoryState &memory, MessageId message, FileId file);
    static std::vector<MessageAttachment> attachmentsForMessage(const storage_detail::MemoryState &memory, MessageId message);
    static bool canAccessFile(const storage_detail::MemoryState &memory, UserId user, FileId file);
};
}
