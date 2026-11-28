#pragma once

#include "backend/application/modules/shared/domain/ApplicationError/ApplicationError.h"
#include "backend/application/modules/contacts/ports/ISocialBusiness/ISocialBusiness.h"
#include "backend/application/modules/shared/ports/IDatabase/IDatabase.h"
#include "backend/application/modules/accounts/ports/IUserRepository/IUserRepository.h"
#include "backend/application/modules/contacts/ports/ISocialRepository/ISocialRepository.h"
#include "backend/application/modules/messaging/ports/IOutboxRepository/IOutboxRepository.h"
#include "backend/application/modules/accounts/domain/AccountTypes/AccountTypes.h"
#include "backend/application/modules/contacts/domain/SocialTypes/SocialTypes.h"

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
