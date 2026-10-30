#include "backend/application/storage/postgres/PostgresMessageWriteRepository/PostgresMessageWriteRepository.h"
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
MessageState messageState(std::string_view state)
{
    if (state == "active") return MessageState::Active;
    if (state == "recalled") return MessageState::Recalled;
    throw std::runtime_error("unknown persisted message state");
}

Message readMessage(const Result &result, int row)
{
    Message message;
    message.id = std::stoull(result.value(row, 0));
    message.conversationId = std::stoull(result.value(row, 1));
    message.sequence = std::stoull(result.value(row, 2));
    message.senderId = std::stoull(result.value(row, 3));
    message.clientMessageId = result.value(row, 4);
    message.body = result.value(row, 5);
    message.modelTrace.nodeId = decodeOptionalId(result, row, 6);
    message.modelTrace.versionId = decodeOptionalId(result, row, 7);
    message.modelTrace.adapterName = result.value(row, 8);
    message.createdAt = decodeTime(result, row, 9);
    message.state = messageState(result.value(row, 10));
    message.recalledBy = decodeOptionalId(result, row, 11);
    message.recalledAt = decodeOptionalTime(result, row, 12);
    return message;
}

constexpr std::string_view kMessageColumns =
    "id, conversation_id, conversation_seq, sender_id, client_message_id, body, "
    "model_node_id, model_version_id, adapter_name, "
    "(extract(epoch FROM created_at) * 1000000)::bigint, state, recalled_by, "
    "(extract(epoch FROM recalled_at) * 1000000)::bigint";

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

Message hydrateMessage(PGconn *connection, Message message)
{
    message.attachments = readAttachments(connection, message.id);
    return message;
}
}
AppendMessageResult PostgresMessageWriteRepository::appendMessage(PGconn *connection, bool ownsTransaction, Message message)
{

    const bool localTransaction = ownsTransaction;
    if (localTransaction) command(connection, "BEGIN");
    try
    {
        (void)query(connection, "SELECT pg_advisory_xact_lock($1::bigint)",
                    {number(message.conversationId)});
        auto existing = query(connection,
            "SELECT " + std::string(kMessageColumns) +
            " FROM messages WHERE conversation_id=$1::bigint AND sender_id=$2::bigint AND client_message_id=$3",
            {number(message.conversationId), number(message.senderId),
             message.clientMessageId});
        if (existing.rows())
        {
            if (localTransaction) command(connection, "COMMIT");
            return {hydrateMessage(connection, readMessage(existing, 0)),
                    false};
        }

        const auto allocated = query(connection,
            "UPDATE conversations SET next_sequence=next_sequence+1 WHERE id=$1::bigint "
            "AND state='active' "
            "RETURNING next_sequence-1", {number(message.conversationId)});
        if (!allocated.rows())
            throw std::invalid_argument("conversation does not exist");
        const auto sequence = allocated.value(0, 0);
        const auto inserted = query(connection,
            "INSERT INTO messages(conversation_id,conversation_seq,sender_id,client_message_id,body,model_node_id,model_version_id,adapter_name,created_at) "
            "VALUES($1::bigint,$2::bigint,$3::bigint,$4,$5,$6::bigint,$7::bigint,$8,to_timestamp($9::double precision/1000000.0)) RETURNING " +
            std::string(kMessageColumns),
            {number(message.conversationId), sequence, number(message.senderId),
             message.clientMessageId, message.body,
             message.modelTrace.nodeId ? std::optional<std::string>{number(*message.modelTrace.nodeId)} : std::nullopt,
             message.modelTrace.versionId ? std::optional<std::string>{number(*message.modelTrace.versionId)} : std::nullopt,
             message.modelTrace.adapterName, encodeTime(message.createdAt)});
        if (localTransaction) command(connection, "COMMIT");
        return {readMessage(inserted, 0), true};
    }
    catch (...)
    {
        if (localTransaction)
        {
            try { command(connection, "ROLLBACK"); } catch (...) {}
        }
        throw;
    }
}

RecallMessageResult PostgresMessageWriteRepository::recallMessage(PGconn *connection, MessageId id, UserId actor, TimePoint now)
{

    const auto changed = query(connection,
        "UPDATE messages SET state='recalled',recalled_by=$2::bigint,"
        "recalled_at=to_timestamp($3::double precision/1000000.0) "
        "WHERE id=$1::bigint AND state='active' RETURNING " +
        std::string(kMessageColumns),
        {number(id), number(actor), encodeTime(now)});
    if (changed.rows())
        return {hydrateMessage(connection, readMessage(changed, 0)), true};
    const auto existing = query(connection,
        "SELECT " + std::string(kMessageColumns) +
        " FROM messages WHERE id=$1::bigint", {number(id)});
    return existing.rows() ? RecallMessageResult{hydrateMessage(
                                 connection, readMessage(existing, 0)), false}
                           : RecallMessageResult{};
}

}
