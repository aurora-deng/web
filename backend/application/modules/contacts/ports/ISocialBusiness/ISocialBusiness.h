#pragma once
#include "backend/application/modules/contacts/domain/SocialTypes/SocialTypes.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webserver::phase11 {

class ISocialBusiness
{
public:
    virtual ~ISocialBusiness() = default;

    virtual FriendRequest requestFriendship(UserId sender, UserId receiver,
                                            TimePoint now) = 0;
    virtual FriendRequest decideFriendship(FriendRequestId request,
                                           UserId receiver, bool accept,
                                           TimePoint now) = 0;
    [[nodiscard]] virtual std::vector<FriendContact> friends(
        UserId user) const = 0;
    [[nodiscard]] virtual std::vector<FriendRequest> pending(
        UserId receiver) const = 0;
    virtual FriendContact updateContact(UserId owner, UserId contact,
                                        std::string remark,
                                        bool notificationsMuted,
                                        TimePoint now) = 0;
    virtual void removeFriend(UserId user, UserId contact, TimePoint now) = 0;
    virtual void block(UserId user, UserId target, TimePoint now) = 0;
    virtual void unblock(UserId user, UserId target, TimePoint now) = 0;
    [[nodiscard]] virtual std::vector<User> blocked(UserId user) const = 0;
    virtual void recordPresence(UserId user, std::size_t connections,
                                TimePoint now) = 0;
};

} // namespace webserver::phase11
