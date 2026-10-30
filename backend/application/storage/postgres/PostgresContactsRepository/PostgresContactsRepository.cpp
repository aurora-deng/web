#include "backend/application/storage/postgres/PostgresContactsRepository/PostgresContactsRepository.h"
#include "backend/application/storage/postgres/PostgresValueCodec/PostgresValueCodec.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace webserver::phase11 {
namespace {
using namespace storage_detail;
std::string friendState(FriendRequestState state)
{
    switch (state)
    {
    case FriendRequestState::Pending: return "pending";
    case FriendRequestState::Accepted: return "accepted";
    case FriendRequestState::Rejected: return "rejected";
    }
    throw std::logic_error("unknown friend request state");
}

FriendRequestState friendState(std::string_view state)
{
    if (state == "pending") return FriendRequestState::Pending;
    if (state == "accepted") return FriendRequestState::Accepted;
    if (state == "rejected") return FriendRequestState::Rejected;
    throw std::runtime_error("unknown persisted friend request state");
}

FriendRequest readFriendRequest(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)),
            std::stoull(result.value(row, 2)),
            friendState(result.value(row, 3)), decodeTime(result, row, 4),
            decodeOptionalTime(result, row, 5)};
}

constexpr std::string_view kFriendColumns =
    "id, sender_id, receiver_id, state, "
    "(extract(epoch FROM created_at) * 1000000)::bigint, "
    "(extract(epoch FROM decided_at) * 1000000)::bigint";
}
FriendRequest PostgresContactsRepository::createFriendRequest(PGconn *connection, FriendRequest request)
{

    const auto result = query(connection,
        "INSERT INTO friend_requests(sender_id,receiver_id,state,created_at) "
        "VALUES($1::bigint,$2::bigint,$3,to_timestamp($4::double precision/1000000.0)) RETURNING " +
        std::string(kFriendColumns),
        {number(request.senderId), number(request.receiverId),
         friendState(request.state), encodeTime(request.createdAt)});
    return readFriendRequest(result, 0);
}

std::optional<FriendRequest> PostgresContactsRepository::findFriendRequest(PGconn *connection, FriendRequestId id)
{

    const auto result = query(connection,
        "SELECT " + std::string(kFriendColumns) +
        " FROM friend_requests WHERE id=$1::bigint", {number(id)});
    return result.rows() ? std::optional<FriendRequest>{readFriendRequest(result, 0)}
                         : std::nullopt;
}

bool PostgresContactsRepository::decideFriendRequest(PGconn *connection, FriendRequestId id, UserId receiver, FriendRequestState decision, TimePoint now)
{

    const auto result = query(connection,
        "WITH decided AS ("
        " UPDATE friend_requests SET state=$3, decided_at=to_timestamp($4::double precision/1000000.0)"
        " WHERE id=$1::bigint AND receiver_id=$2::bigint AND state='pending'"
        " RETURNING sender_id, receiver_id, state"
        "), friendship AS ("
        " INSERT INTO friendships(user_low_id,user_high_id)"
        " SELECT LEAST(sender_id,receiver_id),GREATEST(sender_id,receiver_id) FROM decided"
        " WHERE state='accepted' ON CONFLICT DO NOTHING RETURNING 1"
        ") SELECT count(*) FROM decided",
        {number(id), number(receiver), friendState(decision), encodeTime(now)});
    return result.value(0, 0) == "1";
}

bool PostgresContactsRepository::areFriends(PGconn *connection, UserId first, UserId second)
{

    return query(connection,
        "SELECT 1 FROM friendships WHERE user_low_id=LEAST($1::bigint,$2::bigint) "
        "AND user_high_id=GREATEST($1::bigint,$2::bigint)",
        {number(first), number(second)}).rows() == 1;
}

std::vector<UserId> PostgresContactsRepository::listFriendIds(PGconn *connection, UserId user)
{

    const auto result = query(connection,
        "SELECT CASE WHEN user_low_id=$1::bigint THEN user_high_id ELSE user_low_id END "
        "FROM friendships WHERE user_low_id=$1::bigint OR user_high_id=$1::bigint "
        "ORDER BY 1", {number(user)});
    std::vector<UserId> friends;
    friends.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        friends.push_back(std::stoull(result.value(row, 0)));
    return friends;
}

std::vector<FriendRequest> PostgresContactsRepository::pendingRequests(PGconn *connection, UserId receiver)
{

    const auto result = query(connection,
        "SELECT " + std::string(kFriendColumns) +
        " FROM friend_requests WHERE receiver_id=$1::bigint AND state='pending' ORDER BY id",
        {number(receiver)});
    std::vector<FriendRequest> requests;
    requests.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        requests.push_back(readFriendRequest(result, row));
    return requests;
}

bool PostgresContactsRepository::removeFriendship(PGconn *connection, UserId first, UserId second)
{

    const auto result = query(connection,
        "WITH removed AS ("
        " DELETE FROM friendships WHERE user_low_id=LEAST($1::bigint,$2::bigint)"
        " AND user_high_id=GREATEST($1::bigint,$2::bigint) RETURNING 1"
        "), cleared AS ("
        " DELETE FROM contact_preferences WHERE ("
        " (owner_id=$1::bigint AND contact_id=$2::bigint) OR"
        " (owner_id=$2::bigint AND contact_id=$1::bigint))"
        " AND EXISTS (SELECT 1 FROM removed) RETURNING 1"
        ") SELECT count(*) FROM removed",
        {number(first), number(second)});
    return result.value(0, 0) == "1";
}

ContactSettings PostgresContactsRepository::contactSettings(PGconn *connection, UserId owner, UserId contact)
{

    const auto result = query(connection,
        "SELECT remark,notifications_muted FROM contact_preferences "
        "WHERE owner_id=$1::bigint AND contact_id=$2::bigint",
        {number(owner), number(contact)});
    if (!result.rows())
        return {owner, contact, {}, false};
    return {owner, contact, result.value(0, 0), decodeBool(result, 0, 1)};
}

bool PostgresContactsRepository::updateContactSettings(PGconn *connection, UserId owner, UserId contact, std::string remark, bool notificationsMuted)
{

    return query(connection,
        "INSERT INTO contact_preferences(owner_id,contact_id,remark,notifications_muted) "
        "SELECT $1::bigint,$2::bigint,$3,$4::boolean WHERE EXISTS ("
        " SELECT 1 FROM friendships WHERE user_low_id=LEAST($1::bigint,$2::bigint)"
        " AND user_high_id=GREATEST($1::bigint,$2::bigint)) "
        "ON CONFLICT(owner_id,contact_id) DO UPDATE SET "
        "remark=EXCLUDED.remark,notifications_muted=EXCLUDED.notifications_muted "
        "RETURNING 1",
        {number(owner), number(contact), std::move(remark),
         notificationsMuted ? "true" : "false"}).rows() == 1;
}

bool PostgresContactsRepository::blockUser(PGconn *connection, UserId blocker, UserId blocked, TimePoint now)
{

    const auto result = query(connection,
        "WITH inserted AS ("
        " INSERT INTO user_blocks(blocker_id,blocked_id,created_at)"
        " VALUES($1::bigint,$2::bigint,to_timestamp($3::double precision/1000000.0))"
        " ON CONFLICT DO NOTHING RETURNING 1"
        "), removed AS ("
        " DELETE FROM friendships WHERE user_low_id=LEAST($1::bigint,$2::bigint)"
        " AND user_high_id=GREATEST($1::bigint,$2::bigint)"
        " AND EXISTS (SELECT 1 FROM inserted) RETURNING 1"
        "), cleared AS ("
        " DELETE FROM contact_preferences WHERE"
        " ((owner_id=$1::bigint AND contact_id=$2::bigint) OR"
        "  (owner_id=$2::bigint AND contact_id=$1::bigint))"
        " AND EXISTS (SELECT 1 FROM inserted) RETURNING 1"
        "), rejected AS ("
        " UPDATE friend_requests SET state='rejected',"
        " decided_at=to_timestamp($3::double precision/1000000.0)"
        " WHERE state='pending' AND"
        " ((sender_id=$1::bigint AND receiver_id=$2::bigint) OR"
        "  (sender_id=$2::bigint AND receiver_id=$1::bigint))"
        " AND EXISTS (SELECT 1 FROM inserted) RETURNING 1"
        ") SELECT count(*) FROM inserted",
        {number(blocker), number(blocked), encodeTime(now)});
    return result.value(0, 0) == "1";
}

bool PostgresContactsRepository::unblockUser(PGconn *connection, UserId blocker, UserId blocked)
{

    return query(connection,
        "DELETE FROM user_blocks WHERE blocker_id=$1::bigint "
        "AND blocked_id=$2::bigint RETURNING 1",
        {number(blocker), number(blocked)}).rows() == 1;
}

bool PostgresContactsRepository::isBlocked(PGconn *connection, UserId blocker, UserId blocked)
{

    return query(connection,
        "SELECT 1 FROM user_blocks WHERE blocker_id=$1::bigint "
        "AND blocked_id=$2::bigint",
        {number(blocker), number(blocked)}).rows() == 1;
}

bool PostgresContactsRepository::blockedEitherDirection(PGconn *connection, UserId first, UserId second)
{

    return query(connection,
        "SELECT 1 FROM user_blocks WHERE "
        "(blocker_id=$1::bigint AND blocked_id=$2::bigint) OR "
        "(blocker_id=$2::bigint AND blocked_id=$1::bigint) LIMIT 1",
        {number(first), number(second)}).rows() == 1;
}

std::vector<UserId> PostgresContactsRepository::listBlockedIds(PGconn *connection, UserId blocker)
{

    const auto result = query(connection,
        "SELECT blocked_id FROM user_blocks WHERE blocker_id=$1::bigint "
        "ORDER BY blocked_id", {number(blocker)});
    std::vector<UserId> blocked;
    blocked.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        blocked.push_back(std::stoull(result.value(row, 0)));
    return blocked;
}

bool PostgresContactsRepository::updateLastActive(PGconn *connection, UserId user, TimePoint now)
{

    return query(connection,
        "INSERT INTO user_presence(user_id,last_active_at) "
        "SELECT $1::bigint,to_timestamp($2::double precision/1000000.0) "
        "WHERE EXISTS (SELECT 1 FROM users WHERE id=$1::bigint) "
        "ON CONFLICT(user_id) DO UPDATE SET last_active_at=EXCLUDED.last_active_at "
        "RETURNING 1", {number(user), encodeTime(now)}).rows() == 1;
}

std::optional<TimePoint> PostgresContactsRepository::lastActive(PGconn *connection, UserId user)
{

    const auto result = query(connection,
        "SELECT (extract(epoch FROM last_active_at)*1000000)::bigint "
        "FROM user_presence WHERE user_id=$1::bigint", {number(user)});
    return result.rows() ? std::optional<TimePoint>{decodeTime(result, 0, 0)}
                         : std::nullopt;
}

}
