#include "backend/application/storage/postgres/PostgresAccountRepository/PostgresAccountRepository.h"
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
User readUser(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)), result.value(row, 1),
            result.value(row, 2), result.value(row, 3), result.value(row, 4),
            decodeBool(result, row, 5), decodeBool(result, row, 6),
            decodeTime(result, row, 7), decodeOptionalId(result, row, 8)};
}

Session readSession(const Result &result, int row)
{
    return {result.value(row, 0), std::stoull(result.value(row, 1)),
            result.value(row, 2), result.value(row, 3),
            decodeTime(result, row, 4), decodeTime(result, row, 5),
            decodeOptionalTime(result, row, 6)};
}

constexpr std::string_view kUserColumns =
    "id, username, display_name, biography, password_hash, disabled, ai_account, "
    "(extract(epoch FROM created_at) * 1000000)::bigint, avatar_file_id";

constexpr std::string_view kSessionColumns =
    "id, user_id, token_hash, csrf_hash, "
    "(extract(epoch FROM created_at) * 1000000)::bigint, "
    "(extract(epoch FROM expires_at) * 1000000)::bigint, "
    "(extract(epoch FROM revoked_at) * 1000000)::bigint";
}
User PostgresAccountRepository::create(PGconn *connection, User user)
{

    const auto result = query(connection,
        "INSERT INTO users(username, display_name, biography, password_hash, disabled, ai_account, created_at) "
        "VALUES($1,$2,$3,$4,$5::boolean,$6::boolean,to_timestamp($7::double precision/1000000.0)) RETURNING " +
        std::string(kUserColumns),
        {user.username, user.displayName, user.biography, user.passwordHash,
         user.disabled ? "true" : "false", user.aiAccount ? "true" : "false",
         encodeTime(user.createdAt)});
    return readUser(result, 0);
}

std::optional<User> PostgresAccountRepository::findUser(PGconn *connection, UserId id)
{

    const auto result = query(connection,
        "SELECT " + std::string(kUserColumns) + " FROM users WHERE id=$1::bigint",
        {number(id)});
    return result.rows() ? std::optional<User>{readUser(result, 0)} : std::nullopt;
}

std::optional<User> PostgresAccountRepository::findUserByName(PGconn *connection, std::string_view username)
{

    const auto result = query(connection,
        "SELECT " + std::string(kUserColumns) + " FROM users WHERE username=$1",
        {std::string(username)});
    return result.rows() ? std::optional<User>{readUser(result, 0)} : std::nullopt;
}

bool PostgresAccountRepository::updateProfile(PGconn *connection, UserId id, std::string displayName, std::string biography)
{

    return query(connection,
        "UPDATE users SET display_name=$2, biography=$3 WHERE id=$1::bigint RETURNING 1",
        {number(id), std::move(displayName), std::move(biography)}).rows() == 1;
}

ProfileWriteResult PostgresAccountRepository::writeProfileOnce(PGconn *connection, UserId user, std::string operation, std::string displayName, std::string biography)
{

    const auto exists=query(connection,"SELECT id FROM users WHERE id=$1::bigint",{number(user)});
    if(!exists.rows()){return ProfileWriteResult::NotFound;}
    const auto inserted=query(connection,"INSERT INTO profile_operation_receipts(user_id,operation_id,display_name,biography) VALUES($1::bigint,$2,$3,$4) ON CONFLICT DO NOTHING RETURNING 1",{number(user),operation,displayName,biography});
    ProfileWriteResult result=ProfileWriteResult::Applied;
    if(inserted.rows())command(connection,"UPDATE users SET display_name=$2,biography=$3 WHERE id=$1::bigint",{number(user),displayName,biography});
    else {const auto previous=query(connection,"SELECT display_name,biography FROM profile_operation_receipts WHERE user_id=$1::bigint AND operation_id=$2",{number(user),operation});result=previous.rows()&&previous.value(0,0)==displayName&&previous.value(0,1)==biography?ProfileWriteResult::Replay:ProfileWriteResult::Conflict;}
    return result;
}

bool PostgresAccountRepository::updateAvatar(PGconn *connection, UserId id, std::optional<FileId> avatar)
{

    return query(connection,
        "UPDATE users SET avatar_file_id=$2::bigint WHERE id=$1::bigint "
        "AND ($2::bigint IS NULL OR EXISTS(SELECT 1 FROM file_objects f "
        "WHERE f.id=$2::bigint AND f.owner_id=$1::bigint AND f.state='ready')) "
        "RETURNING 1",
        {number(id), avatar ? std::optional<std::string>{number(*avatar)}
                            : std::nullopt}).rows() == 1;
}

Session PostgresAccountRepository::createSession(PGconn *connection, Session session)
{

    const auto result = query(connection,
        "INSERT INTO user_sessions(id,user_id,token_hash,csrf_hash,created_at,expires_at,revoked_at) "
        "VALUES($1,$2::bigint,$3,$4,to_timestamp($5::double precision/1000000.0),"
        "to_timestamp($6::double precision/1000000.0),NULL) RETURNING " +
        std::string(kSessionColumns),
        {session.id, number(session.userId), session.tokenHash, session.csrfHash,
         encodeTime(session.createdAt), encodeTime(session.expiresAt)});
    return readSession(result, 0);
}

std::optional<Session> PostgresAccountRepository::findSessionByTokenHash(PGconn *connection, std::string_view tokenHash)
{

    const auto result = query(connection,
        "SELECT " + std::string(kSessionColumns) +
        " FROM user_sessions WHERE token_hash=$1", {std::string(tokenHash)});
    return result.rows() ? std::optional<Session>{readSession(result, 0)}
                         : std::nullopt;
}

bool PostgresAccountRepository::revokeSession(PGconn *connection, std::string_view sessionId, TimePoint now)
{

    return query(connection,
        "UPDATE user_sessions SET revoked_at=to_timestamp($2::double precision/1000000.0) "
        "WHERE id=$1 RETURNING 1", {std::string(sessionId), encodeTime(now)}).rows() == 1;
}

}
