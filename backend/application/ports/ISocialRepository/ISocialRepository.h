#pragma once
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include "backend/application/domain/SocialTypes/SocialTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace webserver::phase11 {
class ISocialRepository
{
public:
    virtual ~ISocialRepository() = default;
    virtual FriendRequest createFriendRequest(FriendRequest request) = 0;
    [[nodiscard]] virtual std::optional<FriendRequest> findFriendRequest(
        FriendRequestId id) const = 0;
    virtual bool decideFriendRequest(FriendRequestId id, UserId receiver,
                                     FriendRequestState decision, TimePoint now) = 0;
    [[nodiscard]] virtual bool areFriends(UserId first, UserId second) const = 0;
    [[nodiscard]] virtual std::vector<UserId> listFriendIds(UserId user) const = 0;
    [[nodiscard]] virtual std::vector<FriendRequest> pendingRequests(
        UserId receiver) const = 0;
    virtual bool removeFriendship(UserId first, UserId second) = 0;
    [[nodiscard]] virtual ContactSettings contactSettings(
        UserId owner, UserId contact) const = 0;
    virtual bool updateContactSettings(UserId owner, UserId contact,
                                       std::string remark,
                                       bool notificationsMuted) = 0;
    virtual bool blockUser(UserId blocker, UserId blocked,
                           TimePoint now) = 0;
    virtual bool unblockUser(UserId blocker, UserId blocked) = 0;
    [[nodiscard]] virtual bool isBlocked(UserId blocker,
                                         UserId blocked) const = 0;
    [[nodiscard]] virtual bool blockedEitherDirection(UserId first,
                                                       UserId second) const = 0;
    [[nodiscard]] virtual std::vector<UserId> listBlockedIds(
        UserId blocker) const = 0;
    virtual bool updateLastActive(UserId user, TimePoint now) = 0;
    [[nodiscard]] virtual std::optional<TimePoint> lastActive(
        UserId user) const = 0;
};
}
