#include "backend/application/storage/memory/MemoryContactsRepository/MemoryContactsRepository.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>
namespace webserver::phase11 {
using storage_detail::orderedUsers;
using storage_detail::UserPair;
using storage_detail::MemberKey;
using storage_detail::IdempotencyKey;
FriendRequest MemoryContactsRepository::createFriendRequest(storage_detail::MemoryState &memory, FriendRequest request)
{
    if (!memory.users.contains(request.senderId) ||
        !memory.users.contains(request.receiverId) ||
        request.senderId == request.receiverId)
        throw std::invalid_argument("invalid friend request users");
    request.id = memory.nextFriendRequestId++;
    memory.friendRequests.emplace(request.id, request);
    return request;
}

std::optional<FriendRequest> MemoryContactsRepository::findFriendRequest(const storage_detail::MemoryState &memory, FriendRequestId id)
{
    const auto found = memory.friendRequests.find(id);
    return found == memory.friendRequests.end()
               ? std::nullopt
               : std::optional<FriendRequest>{found->second};
}

bool MemoryContactsRepository::decideFriendRequest(storage_detail::MemoryState &memory, FriendRequestId id, UserId receiver, FriendRequestState decision, TimePoint now)
{
    const auto found = memory.friendRequests.find(id);
    if (found == memory.friendRequests.end() ||
        found->second.receiverId != receiver ||
        found->second.state != FriendRequestState::Pending ||
        decision == FriendRequestState::Pending)
        return false;
    found->second.state = decision;
    found->second.decidedAt = now;
    if (decision == FriendRequestState::Accepted)
        memory.friendships.emplace(
            orderedUsers(found->second.senderId, found->second.receiverId));
    return true;
}

bool MemoryContactsRepository::areFriends(const storage_detail::MemoryState &memory, UserId first, UserId second)
{
    return memory.friendships.contains(orderedUsers(first, second));
}

std::vector<UserId> MemoryContactsRepository::listFriendIds(const storage_detail::MemoryState &memory, UserId user)
{
    std::vector<UserId> result;
    for (const auto &[low, high] : memory.friendships)
    {
        if (low == user)
            result.push_back(high);
        else if (high == user)
            result.push_back(low);
    }
    return result;
}

std::vector<FriendRequest> MemoryContactsRepository::pendingRequests(const storage_detail::MemoryState &memory, UserId receiver)
{
    std::vector<FriendRequest> result;
    for (const auto &[_, request] : memory.friendRequests)
        if (request.receiverId == receiver &&
            request.state == FriendRequestState::Pending)
            result.push_back(request);
    return result;
}

bool MemoryContactsRepository::removeFriendship(storage_detail::MemoryState &memory, UserId first, UserId second)
{
    const auto removed = memory.friendships.erase(orderedUsers(first, second)) != 0;
    if (removed)
    {
        memory.contactSettings.erase({first, second});
        memory.contactSettings.erase({second, first});
    }
    return removed;
}

ContactSettings MemoryContactsRepository::contactSettings(const storage_detail::MemoryState &memory, UserId owner, UserId contact)
{
    const auto found = memory.contactSettings.find({owner, contact});
    return found == memory.contactSettings.end()
               ? ContactSettings{owner, contact, {}, false}
               : found->second;
}

bool MemoryContactsRepository::updateContactSettings(storage_detail::MemoryState &memory, UserId owner, UserId contact, std::string remark, bool notificationsMuted)
{
    if (!memory.friendships.contains(orderedUsers(owner, contact)))
        return false;
    memory.contactSettings[{owner, contact}] =
        ContactSettings{owner, contact, std::move(remark), notificationsMuted};
    return true;
}

bool MemoryContactsRepository::blockUser(storage_detail::MemoryState &memory, UserId blocker, UserId blocked, TimePoint now)
{
    if (blocker == blocked || !memory.users.contains(blocker) ||
        !memory.users.contains(blocked))
        return false;
    const auto inserted = memory.blocks.emplace(blocker, blocked).second;
    if (!inserted)
        return false;

    memory.friendships.erase(orderedUsers(blocker, blocked));
    memory.contactSettings.erase({blocker, blocked});
    memory.contactSettings.erase({blocked, blocker});
    for (auto &[_, request] : memory.friendRequests)
    {
        const bool samePair =
            (request.senderId == blocker && request.receiverId == blocked) ||
            (request.senderId == blocked && request.receiverId == blocker);
        if (samePair && request.state == FriendRequestState::Pending)
        {
            request.state = FriendRequestState::Rejected;
            request.decidedAt = now;
        }
    }
    return true;
}

bool MemoryContactsRepository::unblockUser(storage_detail::MemoryState &memory, UserId blocker, UserId blocked)
{
    return memory.blocks.erase({blocker, blocked}) != 0;
}

bool MemoryContactsRepository::isBlocked(const storage_detail::MemoryState &memory, UserId blocker, UserId blocked)
{
    return memory.blocks.contains({blocker, blocked});
}

bool MemoryContactsRepository::blockedEitherDirection(const storage_detail::MemoryState &memory, UserId first, UserId second)
{
    return memory.blocks.contains({first, second}) ||
           memory.blocks.contains({second, first});
}

std::vector<UserId> MemoryContactsRepository::listBlockedIds(const storage_detail::MemoryState &memory, UserId blocker)
{
    std::vector<UserId> result;
    for (const auto &[owner, blocked] : memory.blocks)
        if (owner == blocker)
            result.push_back(blocked);
    return result;
}

bool MemoryContactsRepository::updateLastActive(storage_detail::MemoryState &memory, UserId user, TimePoint now)
{
    if (!memory.users.contains(user))
        return false;
    memory.lastActive[user] = now;
    return true;
}

std::optional<TimePoint> MemoryContactsRepository::lastActive(const storage_detail::MemoryState &memory, UserId user)
{
    const auto found = memory.lastActive.find(user);
    return found == memory.lastActive.end()
               ? std::nullopt : std::optional<TimePoint>{found->second};
}

}
