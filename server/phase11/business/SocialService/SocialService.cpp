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
    const auto decision = accept ? FriendRequestState::Accepted
                                 : FriendRequestState::Rejected;
    if (!social_.decideFriendRequest(request, receiver, decision, now))
        throw ApplicationError(ErrorCode::Conflict,
                               "friend request was already decided");
    return *social_.findFriendRequest(request);
}

std::vector<User> SocialService::friends(UserId user) const
{
    if (!users_.findUser(user))
        throw ApplicationError(ErrorCode::NotFound, "user not found");
    std::vector<User> result;
    for (const auto friendId : social_.listFriendIds(user))
        if (const auto friendUser = users_.findUser(friendId))
            result.push_back(*friendUser);
    return result;
}

std::vector<FriendRequest> SocialService::pending(UserId receiver) const
{
    if (!users_.findUser(receiver))
        throw ApplicationError(ErrorCode::NotFound, "user not found");
    return social_.pendingRequests(receiver);
}

} // namespace webserver::phase11
