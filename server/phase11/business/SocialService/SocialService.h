#pragma once

#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/business/BusinessInterfaces/BusinessInterfaces.h"
#include "server/phase11/ports/Ports/Ports.h"

namespace webserver::phase11
{

class SocialService final : public ISocialBusiness
{
public:
    SocialService(IDatabase &database, IUserRepository &users,
                  ISocialRepository &social, IOutboxRepository &outbox)
        : database_(database), users_(users), social_(social), outbox_(outbox)
    {
    }

    FriendRequest requestFriendship(UserId sender, UserId receiver,
                                    TimePoint now) override;
    FriendRequest decideFriendship(FriendRequestId request, UserId receiver,
                                   bool accept, TimePoint now) override;
    std::vector<FriendContact> friends(UserId user) const override;
    std::vector<FriendRequest> pending(UserId receiver) const override;
    FriendContact updateContact(UserId owner, UserId contact,
                                std::string remark,
                                bool notificationsMuted,
                                TimePoint now) override;
    void removeFriend(UserId user, UserId contact, TimePoint now) override;
    void block(UserId user, UserId target, TimePoint now) override;
    void unblock(UserId user, UserId target, TimePoint now) override;
    std::vector<User> blocked(UserId user) const override;
    void recordPresence(UserId user, std::size_t connections,
                        TimePoint now) override;

private:
    IDatabase &database_;
    IUserRepository &users_;
    ISocialRepository &social_;
    IOutboxRepository &outbox_;
};

} // namespace webserver::phase11
