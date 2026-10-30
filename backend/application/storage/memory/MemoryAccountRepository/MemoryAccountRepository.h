#pragma once
#include "backend/application/storage/memory/MemoryState/MemoryState.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
namespace webserver::phase11 {
/** Account in-memory adapter. Facade holds the shared lock during calls. */
class MemoryAccountRepository final {
public:
    static User create(storage_detail::MemoryState &memory, User user);
    static std::optional<User> findUser(const storage_detail::MemoryState &memory, UserId id);
    static std::optional<User> findUserByName(const storage_detail::MemoryState &memory, std::string_view username);
    static bool updateProfile(storage_detail::MemoryState &memory, UserId id, std::string displayName, std::string biography);
    static ProfileWriteResult writeProfileOnce(storage_detail::MemoryState &memory, UserId id, std::string operation, std::string displayName, std::string biography);
    static bool updateAvatar(storage_detail::MemoryState &memory, UserId id, std::optional<FileId> avatar);
    static Session createSession(storage_detail::MemoryState &memory, Session session);
    static std::optional<Session> findSessionByTokenHash(const storage_detail::MemoryState &memory, std::string_view tokenHash);
    static bool revokeSession(storage_detail::MemoryState &memory, std::string_view sessionId, TimePoint now);
};
}
