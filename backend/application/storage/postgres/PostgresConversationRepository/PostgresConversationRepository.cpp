#include "backend/application/storage/postgres/PostgresConversationRepository/PostgresConversationRepository.h"
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
std::string conversationKind(ConversationKind kind)
{
    return kind == ConversationKind::Direct ? "direct" : "group";
}

ConversationKind conversationKind(std::string_view kind)
{
    if (kind == "direct") return ConversationKind::Direct;
    if (kind == "group") return ConversationKind::Group;
    throw std::runtime_error("unknown persisted conversation kind");
}

std::string memberRole(MemberRole role)
{
    switch (role)
    {
    case MemberRole::Member: return "member";
    case MemberRole::Administrator: return "administrator";
    case MemberRole::Owner: return "owner";
    }
    throw std::logic_error("unknown member role");
}

MemberRole memberRole(std::string_view role)
{
    if (role == "owner") return MemberRole::Owner;
    if (role == "administrator") return MemberRole::Administrator;
    if (role == "member") return MemberRole::Member;
    throw std::runtime_error("unknown persisted member role");
}

std::string conversationState(ConversationState state)
{
    return state == ConversationState::Active ? "active" : "dissolved";
}

ConversationState conversationState(std::string_view state)
{
    if (state == "active") return ConversationState::Active;
    if (state == "dissolved") return ConversationState::Dissolved;
    throw std::runtime_error("unknown persisted conversation state");
}

Conversation readConversation(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            conversationKind(result.value(row, 1)), result.value(row, 2),
            std::stoull(result.value(row, 3)),
            std::stoull(result.value(row, 4)), decodeTime(result, row, 5),
            result.value(row, 6), conversationState(result.value(row, 7)),
            decodeOptionalId(result, row, 8),
            decodeOptionalTime(result, row, 9)};
}

ConversationMember readMember(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)), memberRole(result.value(row, 2)),
            std::stoull(result.value(row, 3)),
            std::stoull(result.value(row, 4)), decodeTime(result, row, 5),
            decodeOptionalTime(result, row, 6)};
}

constexpr std::string_view kConversationColumns =
    "id, kind, title, created_by, next_sequence, "
    "(extract(epoch FROM created_at) * 1000000)::bigint, announcement, state, "
    "dissolved_by, (extract(epoch FROM dissolved_at) * 1000000)::bigint";

constexpr std::string_view kQualifiedConversationColumns =
    "c.id, c.kind, c.title, c.created_by, c.next_sequence, "
    "(extract(epoch FROM c.created_at) * 1000000)::bigint, c.announcement, c.state, "
    "c.dissolved_by, (extract(epoch FROM c.dissolved_at) * 1000000)::bigint";

constexpr std::string_view kMemberColumns =
    "conversation_id, user_id, role, last_delivered_sequence, "
    "last_read_sequence, (extract(epoch FROM joined_at) * 1000000)::bigint, "
    "(extract(epoch FROM muted_until) * 1000000)::bigint";
}
Conversation PostgresConversationRepository::createConversation(PGconn *connection, bool ownsTransaction, Conversation conversation, std::vector<ConversationMember> members)
{

    const bool localTransaction = ownsTransaction;
    if (localTransaction) command(connection, "BEGIN");
    try
    {
        std::optional<std::string> directKey;
        if (conversation.kind == ConversationKind::Direct)
        {
            if (members.size() != 2)
                throw std::invalid_argument("direct conversation needs two members");
            const auto low = std::min(members[0].userId, members[1].userId);
            const auto high = std::max(members[0].userId, members[1].userId);
            directKey = number(low) + ":" + number(high);
        }
        const auto result = query(connection,
            "INSERT INTO conversations(kind,title,created_by,direct_pair_key,created_at,announcement,state) "
            "VALUES($1,$2,$3::bigint,$4,to_timestamp($5::double precision/1000000.0),$6,$7) RETURNING " +
            std::string(kConversationColumns),
            {conversationKind(conversation.kind), conversation.title,
             number(conversation.createdBy), directKey, encodeTime(conversation.createdAt),
             conversation.announcement, conversationState(conversation.state)});
        auto created = readConversation(result, 0);
        for (const auto &member : members)
            (void)query(connection,
                "INSERT INTO conversation_members(conversation_id,user_id,role,last_delivered_sequence,last_read_sequence,joined_at,muted_until) "
                "VALUES($1::bigint,$2::bigint,$3,$4::bigint,$5::bigint,to_timestamp($6::double precision/1000000.0),"
                "CASE WHEN $7::text IS NULL THEN NULL ELSE to_timestamp($7::double precision/1000000.0) END) RETURNING 1",
                {number(created.id), number(member.userId), memberRole(member.role),
                 number(member.lastDeliveredSequence), number(member.lastReadSequence),
                 encodeTime(member.joinedAt),
                 member.mutedUntil ? std::optional<std::string>{encodeTime(*member.mutedUntil)}
                                   : std::nullopt});
        if (localTransaction) command(connection, "COMMIT");
        return created;
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

std::optional<Conversation> PostgresConversationRepository::findConversation(PGconn *connection, bool transactionActive, ConversationId id)
{

    const auto result = query(connection,
        "SELECT " + std::string(kConversationColumns) +
        " FROM conversations WHERE id=$1::bigint" +
        (transactionActive ? " FOR UPDATE" : ""), {number(id)});
    return result.rows() ? std::optional<Conversation>{readConversation(result, 0)}
                         : std::nullopt;
}

std::optional<ConversationMember> PostgresConversationRepository::findMember(PGconn *connection, ConversationId conversation, UserId user)
{

    const auto result = query(connection,
        "SELECT " + std::string(kMemberColumns) +
        " FROM conversation_members WHERE conversation_id=$1::bigint AND user_id=$2::bigint",
        {number(conversation), number(user)});
    return result.rows()
               ? std::optional<ConversationMember>{readMember(result, 0)}
               : std::nullopt;
}

std::optional<Conversation> PostgresConversationRepository::findDirectConversation(PGconn *connection, UserId first, UserId second)
{

    const auto key = number(std::min(first, second)) + ":" +
                     number(std::max(first, second));
    const auto result = query(connection,
        "SELECT " + std::string(kConversationColumns) +
        " FROM conversations WHERE direct_pair_key=$1 AND state='active'", {key});
    return result.rows() ? std::optional<Conversation>{readConversation(result, 0)}
                         : std::nullopt;
}

std::vector<Conversation> PostgresConversationRepository::listConversations(PGconn *connection, UserId user)
{

    const auto result = query(connection,
        "SELECT " + std::string(kQualifiedConversationColumns) +
        " FROM conversations c JOIN conversation_members m ON m.conversation_id=c.id "
        "WHERE m.user_id=$1::bigint AND c.state='active' ORDER BY c.id", {number(user)});
    std::vector<Conversation> conversations;
    conversations.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        conversations.push_back(readConversation(result, row));
    return conversations;
}

std::vector<ConversationSummary> PostgresConversationRepository::listConversationSummaries(PGconn *connection, UserId user)
{

    const auto result = query(connection,
        "SELECT " + std::string(kQualifiedConversationColumns) +
        ",m.last_delivered_sequence,m.last_read_sequence,"
        "GREATEST(c.next_sequence-1-m.last_read_sequence,0),"
        "CASE WHEN c.kind='direct' THEN COALESCE(NULLIF(cp.remark,''),peer.display_name,'') ELSE c.title END,"
        "peer.avatar_file_id,COALESCE(latest.preview,''),"
        "COALESCE((extract(epoch FROM latest.created_at)*1000000)::bigint,0),"
        "COALESCE(pref.notifications_muted,cp.notifications_muted,false),"
        "EXISTS(SELECT 1 FROM conversation_members am JOIN users au ON au.id=am.user_id "
        "WHERE am.conversation_id=c.id AND au.ai_account),peer.id,COALESCE(pref.revision,0) "
        "FROM conversations c JOIN conversation_members m ON m.conversation_id=c.id "
        // LATERAL 读取最新消息可使用现有 (conversation_id,conversation_seq) 索引。
        // 所有会话一次往返；HTTP 层不再逐会话查用户/最新消息。
        "LEFT JOIN LATERAL (SELECT u.id,u.display_name,u.avatar_file_id "
        "FROM conversation_members pm JOIN users u ON u.id=pm.user_id "
        "WHERE c.kind='direct' AND pm.conversation_id=c.id AND pm.user_id<>$1::bigint "
        "ORDER BY pm.user_id LIMIT 1) peer ON true "
        "LEFT JOIN contact_preferences cp ON cp.owner_id=$1::bigint AND cp.contact_id=peer.id "
        "LEFT JOIN LATERAL (SELECT CASE WHEN state='recalled' THEN '这条消息已被撤回' "
        "WHEN body='' THEN '[附件]' ELSE left(body,120) END AS preview,created_at "
        "FROM messages WHERE conversation_id=c.id ORDER BY conversation_seq DESC LIMIT 1) latest ON true "
        "LEFT JOIN conversation_preferences pref ON pref.conversation_id=c.id AND pref.user_id=$1 "
        "WHERE m.user_id=$1::bigint AND c.state='active' "
        "ORDER BY COALESCE(latest.created_at,c.created_at) DESC,c.id DESC", {number(user)});
    std::vector<ConversationSummary> summaries;
    summaries.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row) {
        ConversationSummary summary{readConversation(result, row),
                             std::stoull(result.value(row, 10)),
                             std::stoull(result.value(row, 11)),
                             std::stoull(result.value(row, 12))};
        summary.displayTitle = result.value(row, 13);
        summary.avatarFileId = decodeOptionalId(result, row, 14);
        summary.lastMessagePreview = result.value(row, 15);
        summary.lastMessageAt = decodeTime(result, row, 16);
        summary.notificationsMuted = decodeBool(result, row, 17);
        summary.containsAi = decodeBool(result, row, 18);
        summary.peerUserId=decodeOptionalId(result,row,19);
        summary.preferencesRevision=std::stoull(result.value(row,20));
        summaries.push_back(std::move(summary));
    }
    return summaries;
}

std::vector<ConversationMember> PostgresConversationRepository::listMembers(PGconn *connection, ConversationId conversation)
{

    const auto result = query(connection,
        "SELECT " + std::string(kMemberColumns) + " "
        "FROM conversation_members WHERE conversation_id=$1::bigint ORDER BY user_id",
        {number(conversation)});
    std::vector<ConversationMember> members;
    members.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        members.push_back(readMember(result, row));
    return members;
}

bool PostgresConversationRepository::addMember(PGconn *connection, ConversationMember member)
{

    return query(connection,
        "INSERT INTO conversation_members(conversation_id,user_id,role,last_delivered_sequence,last_read_sequence,joined_at,muted_until) "
        "SELECT $1::bigint,$2::bigint,$3,$4::bigint,$5::bigint,to_timestamp($6::double precision/1000000.0),"
        "CASE WHEN $7::text IS NULL THEN NULL ELSE to_timestamp($7::double precision/1000000.0) END "
        "WHERE EXISTS(SELECT 1 FROM conversations WHERE id=$1::bigint AND state='active') "
        "ON CONFLICT DO NOTHING RETURNING 1",
        {number(member.conversationId), number(member.userId), memberRole(member.role),
         number(member.lastDeliveredSequence), number(member.lastReadSequence),
         encodeTime(member.joinedAt),
         member.mutedUntil ? std::optional<std::string>{encodeTime(*member.mutedUntil)}
                           : std::nullopt}).rows() == 1;
}

bool PostgresConversationRepository::updateConversationProfile(PGconn *connection, ConversationId conversation, std::string title, std::string announcement)
{

    return query(connection,
        "UPDATE conversations SET title=$2,announcement=$3 "
        "WHERE id=$1::bigint AND kind='group' AND state='active' RETURNING 1",
        {number(conversation), std::move(title), std::move(announcement)}).rows() == 1;
}

bool PostgresConversationRepository::dissolveConversation(PGconn *connection, ConversationId conversation, UserId actor, TimePoint now)
{

    return query(connection,
        "UPDATE conversations SET state='dissolved',dissolved_by=$2::bigint,"
        "dissolved_at=to_timestamp($3::double precision/1000000.0) "
        "WHERE id=$1::bigint AND kind='group' AND state='active' RETURNING 1",
        {number(conversation), number(actor), encodeTime(now)}).rows() == 1;
}

bool PostgresConversationRepository::updateMemberRole(PGconn *connection, ConversationId conversation, UserId user, MemberRole role)
{

    return query(connection,
        "UPDATE conversation_members SET role=$3 "
        "WHERE conversation_id=$1::bigint AND user_id=$2::bigint RETURNING 1",
        {number(conversation), number(user), memberRole(role)}).rows() == 1;
}

bool PostgresConversationRepository::updateMemberMute(PGconn *connection, ConversationId conversation, UserId user, std::optional<TimePoint> mutedUntil)
{

    const std::optional<std::string> value = mutedUntil
        ? std::optional<std::string>{encodeTime(*mutedUntil)} : std::nullopt;
    return query(connection,
        "UPDATE conversation_members SET muted_until="
        "CASE WHEN $3::text IS NULL THEN NULL ELSE to_timestamp($3::double precision/1000000.0) END "
        "WHERE conversation_id=$1::bigint AND user_id=$2::bigint RETURNING 1",
        {number(conversation), number(user), value}).rows() == 1;
}

bool PostgresConversationRepository::removeMember(PGconn *connection, ConversationId conversation, UserId user)
{

    return query(connection,
        "DELETE FROM conversation_members WHERE conversation_id=$1::bigint "
        "AND user_id=$2::bigint RETURNING 1",
        {number(conversation), number(user)}).rows() == 1;
}

bool PostgresConversationRepository::updateDelivered(PGconn *connection, ConversationId conversation, UserId user, std::uint64_t sequence)
{

    return query(connection,
        "UPDATE conversation_members SET last_delivered_sequence=GREATEST(last_delivered_sequence,$3::bigint) "
        "WHERE conversation_id=$1::bigint AND user_id=$2::bigint "
        "AND last_delivered_sequence<$3::bigint RETURNING 1",
        {number(conversation), number(user), number(sequence)}).rows() == 1;
}

bool PostgresConversationRepository::updateRead(PGconn *connection, ConversationId conversation, UserId user, std::uint64_t sequence)
{

    return query(connection,
        "UPDATE conversation_members SET last_read_sequence=GREATEST(last_read_sequence,$3::bigint), "
        "last_delivered_sequence=GREATEST(last_delivered_sequence,$3::bigint) "
        "WHERE conversation_id=$1::bigint AND user_id=$2::bigint "
        "AND last_read_sequence<$3::bigint RETURNING 1",
        {number(conversation), number(user), number(sequence)}).rows() == 1;
}

}
