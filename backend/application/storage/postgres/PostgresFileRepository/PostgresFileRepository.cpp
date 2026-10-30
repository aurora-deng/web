#include "backend/application/storage/postgres/PostgresFileRepository/PostgresFileRepository.h"
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
std::string fileState(FileState state)
{
    switch (state)
    {
    case FileState::Pending: return "pending";
    case FileState::Ready: return "ready";
    case FileState::Failed: return "failed";
    case FileState::Deleted: return "deleted";
    }
    throw std::logic_error("unknown file state");
}

FileState fileState(std::string_view state)
{
    if (state == "pending") return FileState::Pending;
    if (state == "ready") return FileState::Ready;
    if (state == "failed") return FileState::Failed;
    if (state == "deleted") return FileState::Deleted;
    throw std::runtime_error("unknown persisted file state");
}

FileObject readFile(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)), result.value(row, 2),
            result.value(row, 3), std::stoull(result.value(row, 4)),
            std::stoull(result.value(row, 5)), result.value(row, 6),
            fileState(result.value(row, 7)), decodeTime(result, row, 8),
            decodeOptionalTime(result, row, 9)};
}

constexpr std::string_view kFileColumns =
    "id, owner_id, original_name, media_type, expected_size, uploaded_size, "
    "sha256, state, (extract(epoch FROM created_at) * 1000000)::bigint, "
    "(extract(epoch FROM ready_at) * 1000000)::bigint";

std::vector<MessageAttachment> readAttachments(PGconn *connection,
                                                MessageId message)
{
    const auto result = query(connection,
        "SELECT f.id,f.original_name,f.media_type,f.expected_size,f.sha256 "
        "FROM message_attachments a JOIN file_objects f ON f.id=a.file_id "
        "WHERE a.message_id=$1::bigint AND f.state='ready' "
        "ORDER BY a.position", {number(message)});
    std::vector<MessageAttachment> attachments;
    attachments.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        attachments.push_back({std::stoull(result.value(row, 0)),
                               result.value(row, 1), result.value(row, 2),
                               std::stoull(result.value(row, 3)),
                               result.value(row, 4)});
    return attachments;
}
}
FileObject PostgresFileRepository::createFile(PGconn *connection, FileObject file)
{

    const auto result = query(connection,
        "INSERT INTO file_objects(owner_id,original_name,media_type,expected_size,"
        "uploaded_size,sha256,state,created_at,ready_at) "
        "VALUES($1::bigint,$2,$3,$4::bigint,$5::bigint,$6,$7,"
        "to_timestamp($8::double precision/1000000.0),NULL) RETURNING " +
        std::string(kFileColumns),
        {number(file.ownerId), file.originalName, file.mediaType,
         number(file.expectedSize), number(file.uploadedSize), file.sha256,
         fileState(file.state), encodeTime(file.createdAt)});
    return readFile(result, 0);
}

std::optional<FileObject> PostgresFileRepository::findFile(PGconn *connection, FileId id)
{

    const auto result = query(connection,
        "SELECT " + std::string(kFileColumns) +
        " FROM file_objects WHERE id=$1::bigint", {number(id)});
    return result.rows() ? std::optional<FileObject>{readFile(result, 0)}
                         : std::nullopt;
}

bool PostgresFileRepository::updateFileProgress(PGconn *connection, FileId id, UserId owner, std::uint64_t expectedOffset, std::uint64_t uploadedSize)
{

    return query(connection,
        "UPDATE file_objects SET uploaded_size=$4::bigint "
        "WHERE id=$1::bigint AND owner_id=$2::bigint AND state='pending' "
        "AND uploaded_size=$3::bigint AND $4::bigint BETWEEN $3::bigint AND expected_size "
        "RETURNING 1",
        {number(id), number(owner), number(expectedOffset),
         number(uploadedSize)}).rows() == 1;
}

bool PostgresFileRepository::completeFile(PGconn *connection, FileId id, UserId owner, std::string sha256, TimePoint now)
{

    return query(connection,
        "UPDATE file_objects SET state='ready',sha256=$3,"
        "ready_at=to_timestamp($4::double precision/1000000.0) "
        "WHERE id=$1::bigint AND owner_id=$2::bigint AND state='pending' "
        "AND uploaded_size=expected_size AND length($3)=64 RETURNING 1",
        {number(id), number(owner), std::move(sha256), encodeTime(now)}).rows() == 1;
}

bool PostgresFileRepository::failFile(PGconn *connection, FileId id, UserId owner)
{

    return query(connection,
        "UPDATE file_objects SET state='failed' WHERE id=$1::bigint "
        "AND owner_id=$2::bigint AND state='pending' RETURNING 1",
        {number(id), number(owner)}).rows() == 1;
}

bool PostgresFileRepository::attachFile(PGconn *connection, MessageId message, FileId file)
{

    return query(connection,
        "INSERT INTO message_attachments(message_id,file_id,position) "
        "SELECT m.id,f.id,0 FROM messages m JOIN file_objects f ON f.id=$2::bigint "
        "WHERE m.id=$1::bigint AND f.state='ready' AND f.owner_id=m.sender_id "
        "AND NOT EXISTS(SELECT 1 FROM message_attachments a WHERE a.message_id=m.id) "
        "RETURNING 1", {number(message), number(file)}).rows() == 1;
}

std::vector<MessageAttachment> PostgresFileRepository::attachmentsForMessage(PGconn *connection, MessageId message)
{

    return readAttachments(connection, message);
}

bool PostgresFileRepository::canAccessFile(PGconn *connection, UserId user, FileId file)
{

    return query(connection,
        "SELECT 1 FROM file_objects f WHERE f.id=$2::bigint AND f.state='ready' "
        "AND (f.owner_id=$1::bigint "
        "OR EXISTS(SELECT 1 FROM users u WHERE u.avatar_file_id=f.id) "
        "OR EXISTS(SELECT 1 FROM message_attachments a "
        "JOIN messages m ON m.id=a.message_id "
        "JOIN conversation_members cm ON cm.conversation_id=m.conversation_id "
        "WHERE a.file_id=f.id AND cm.user_id=$1::bigint)) LIMIT 1",
        {number(user), number(file)}).rows() == 1;
}

}
