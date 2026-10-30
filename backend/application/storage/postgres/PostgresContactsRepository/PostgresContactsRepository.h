#pragma once
#include "backend/application/domain/SocialTypes/SocialTypes.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
#include "backend/application/storage/postgres/PostgresOutboxWriter/PostgresOutboxWriter.h"
namespace webserver::phase11 {
/** Contacts SQL adapter. Uses the caller's connection; never borrows another lease. */
class PostgresContactsRepository final {
public:
    static FriendRequest createFriendRequest(PGconn *connection, FriendRequest request);
    static std::optional<FriendRequest> findFriendRequest(PGconn *connection, FriendRequestId id);
    static bool decideFriendRequest(PGconn *connection, FriendRequestId id, UserId receiver, FriendRequestState decision, TimePoint now);
    static bool areFriends(PGconn *connection, UserId first, UserId second);
    static std::vector<UserId> listFriendIds(PGconn *connection, UserId user);
    static std::vector<FriendRequest> pendingRequests(PGconn *connection, UserId receiver);
    static bool removeFriendship(PGconn *connection, UserId first, UserId second);
    static ContactSettings contactSettings(PGconn *connection, UserId owner, UserId contact);
    static bool updateContactSettings(PGconn *connection, UserId owner, UserId contact, std::string remark, bool notificationsMuted);
    static bool blockUser(PGconn *connection, UserId blocker, UserId blocked, TimePoint now);
    static bool unblockUser(PGconn *connection, UserId blocker, UserId blocked);
    static bool isBlocked(PGconn *connection, UserId blocker, UserId blocked);
    static bool blockedEitherDirection(PGconn *connection, UserId first, UserId second);
    static std::vector<UserId> listBlockedIds(PGconn *connection, UserId blocker);
    static bool updateLastActive(PGconn *connection, UserId user, TimePoint now);
    static std::optional<TimePoint> lastActive(PGconn *connection, UserId user);
};
}
