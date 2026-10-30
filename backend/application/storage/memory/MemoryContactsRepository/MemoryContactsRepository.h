#pragma once
#include "backend/application/storage/memory/MemoryState/MemoryState.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
namespace webserver::phase11 {
/** Contacts in-memory adapter. Facade holds the shared lock during calls. */
class MemoryContactsRepository final {
public:
    static FriendRequest createFriendRequest(storage_detail::MemoryState &memory, FriendRequest request);
    static std::optional<FriendRequest> findFriendRequest(const storage_detail::MemoryState &memory, FriendRequestId id);
    static bool decideFriendRequest(storage_detail::MemoryState &memory, FriendRequestId id, UserId receiver, FriendRequestState decision, TimePoint now);
    static bool areFriends(const storage_detail::MemoryState &memory, UserId first, UserId second);
    static std::vector<UserId> listFriendIds(const storage_detail::MemoryState &memory, UserId user);
    static std::vector<FriendRequest> pendingRequests(const storage_detail::MemoryState &memory, UserId receiver);
    static bool removeFriendship(storage_detail::MemoryState &memory, UserId first, UserId second);
    static ContactSettings contactSettings(const storage_detail::MemoryState &memory, UserId owner, UserId contact);
    static bool updateContactSettings(storage_detail::MemoryState &memory, UserId owner, UserId contact, std::string remark, bool notificationsMuted);
    static bool blockUser(storage_detail::MemoryState &memory, UserId blocker, UserId blocked, TimePoint now);
    static bool unblockUser(storage_detail::MemoryState &memory, UserId blocker, UserId blocked);
    static bool isBlocked(const storage_detail::MemoryState &memory, UserId blocker, UserId blocked);
    static bool blockedEitherDirection(const storage_detail::MemoryState &memory, UserId first, UserId second);
    static std::vector<UserId> listBlockedIds(const storage_detail::MemoryState &memory, UserId blocker);
    static bool updateLastActive(storage_detail::MemoryState &memory, UserId user, TimePoint now);
    static std::optional<TimePoint> lastActive(const storage_detail::MemoryState &memory, UserId user);
};
}
