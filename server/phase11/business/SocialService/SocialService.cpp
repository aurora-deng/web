#include "server/phase11/business/SocialService/SocialService.h"

namespace webserver::phase11
{

FriendRequest SocialService::requestFriendship(UserId sender, UserId receiver,
                                                TimePoint now)
{
    if (sender == receiver)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "cannot add yourself as a friend");
    if (!users_.findUser(sender) || !users_.findUser(receiver))
        throw ApplicationError(ErrorCode::NotFound, "user not found");
    if (social_.blockedEitherDirection(sender, receiver))
        throw ApplicationError(ErrorCode::Forbidden,
                               "friend request is blocked by a privacy rule");
    if (social_.areFriends(sender, receiver))
        throw ApplicationError(ErrorCode::Conflict, "users are already friends");
    for (const auto &pending : social_.pendingRequests(receiver))
        if (pending.senderId == sender)
            throw ApplicationError(ErrorCode::Conflict,
                                   "friend request is already pending");
    // 反向申请也算同一段待处理关系。否则 Alice→Bob 与 Bob→Alice 会各留一张申请单，
    // 随后可能产生互相矛盾的接受/拒绝结果。
    for (const auto &pending : social_.pendingRequests(sender))
        if (pending.senderId == receiver)
            throw ApplicationError(ErrorCode::Conflict,
                                   "reverse friend request is already pending");
    auto transaction = database_.beginTransaction();
    const auto created = social_.createFriendRequest(
        FriendRequest{0, sender, receiver, FriendRequestState::Pending, now, {}});
    outbox_.appendOutbox({0, "friend.request", "friend_request", created.id,
                          std::to_string(receiver), now, {}});
    transaction->commit();
    return created;
}

FriendRequest SocialService::decideFriendship(FriendRequestId request,
                                               UserId receiver,
                                               bool accept,
                                               TimePoint now)
{
    const auto before = social_.findFriendRequest(request);
    if (!before)
        throw ApplicationError(ErrorCode::NotFound, "friend request not found");
    if (before->receiverId != receiver)
        throw ApplicationError(ErrorCode::Forbidden,
                               "only the receiver can decide this request");
    if (accept && social_.blockedEitherDirection(before->senderId,
                                                 before->receiverId))
        throw ApplicationError(ErrorCode::Forbidden,
                               "blocked users cannot become friends");
    const auto decision = accept ? FriendRequestState::Accepted
                                 : FriendRequestState::Rejected;
    auto transaction = database_.beginTransaction();
    if (!social_.decideFriendRequest(request, receiver, decision, now))
        throw ApplicationError(ErrorCode::Conflict,
                               "friend request was already decided");
    outbox_.appendOutbox({0, "friend.decided", "friend_request", request,
                          {}, now, {}});
    transaction->commit();
    return *social_.findFriendRequest(request);
}

std::vector<FriendContact> SocialService::friends(UserId user) const
{
    if (!users_.findUser(user))
        throw ApplicationError(ErrorCode::NotFound, "user not found");
    std::vector<FriendContact> result;
    for (const auto friendId : social_.listFriendIds(user))
        if (const auto friendUser = users_.findUser(friendId))
            result.push_back({*friendUser,
                              social_.contactSettings(user, friendId),
                              social_.lastActive(friendId)});
    return result;
}

std::vector<FriendRequest> SocialService::pending(UserId receiver) const
{
    if (!users_.findUser(receiver))
        throw ApplicationError(ErrorCode::NotFound, "user not found");
    return social_.pendingRequests(receiver);
}

FriendContact SocialService::updateContact(UserId owner, UserId contact,
                                           std::string remark,
                                           bool notificationsMuted,
                                           TimePoint now)
{
    if (remark.size() > 64)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "contact remark is too long");
    const auto contactUser = users_.findUser(contact);
    if (!contactUser)
        throw ApplicationError(ErrorCode::NotFound, "contact not found");
    if (!social_.areFriends(owner, contact))
        throw ApplicationError(ErrorCode::Forbidden,
                               "contact settings require friendship");

    auto transaction = database_.beginTransaction();
    if (!social_.updateContactSettings(owner, contact, std::move(remark),
                                       notificationsMuted))
        throw ApplicationError(ErrorCode::Conflict,
                               "friendship changed while updating contact");
    outbox_.appendOutbox({0, "friend.updated", "user", owner,
                          std::to_string(contact), now, {}});
    transaction->commit();
    return {*contactUser, social_.contactSettings(owner, contact),
            social_.lastActive(contact)};
}

void SocialService::removeFriend(UserId user, UserId contact, TimePoint now)
{
    if (user == contact)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "cannot remove yourself");
    auto transaction = database_.beginTransaction();
    if (!social_.removeFriendship(user, contact))
        throw ApplicationError(ErrorCode::NotFound, "friendship not found");
    outbox_.appendOutbox({0, "friend.removed", "user", user,
                          std::to_string(contact), now, {}});
    transaction->commit();
}

void SocialService::block(UserId user, UserId target, TimePoint now)
{
    if (user == target)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "cannot block yourself");
    if (!users_.findUser(user) || !users_.findUser(target))
        throw ApplicationError(ErrorCode::NotFound, "user not found");
    auto transaction = database_.beginTransaction();
    if (!social_.blockUser(user, target, now))
        throw ApplicationError(ErrorCode::Conflict, "user is already blocked");
    outbox_.appendOutbox({0, "friend.blocked", "user", user,
                          std::to_string(target), now, {}});
    transaction->commit();
}

void SocialService::unblock(UserId user, UserId target, TimePoint now)
{
    auto transaction = database_.beginTransaction();
    if (!social_.unblockUser(user, target))
        throw ApplicationError(ErrorCode::NotFound, "block relationship not found");
    outbox_.appendOutbox({0, "friend.unblocked", "user", user,
                          std::to_string(target), now, {}});
    transaction->commit();
}

std::vector<User> SocialService::blocked(UserId user) const
{
    if (!users_.findUser(user))
        throw ApplicationError(ErrorCode::NotFound, "user not found");
    std::vector<User> result;
    for (const auto blockedId : social_.listBlockedIds(user))
        if (const auto blockedUser = users_.findUser(blockedId))
            result.push_back(*blockedUser);
    return result;
}

void SocialService::recordPresence(UserId user, std::size_t connections,
                                   TimePoint now)
{
    auto transaction = database_.beginTransaction();
    if (!social_.updateLastActive(user, now))
        throw ApplicationError(ErrorCode::NotFound, "presence user not found");
    outbox_.appendOutbox({0, "presence.updated", "user", user,
                          std::to_string(connections), now, {}});
    transaction->commit();
}

} // namespace webserver::phase11
