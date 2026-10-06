#include "server/phase11/storage/postgres/PostgresStore/PostgresStore.h"

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

namespace webserver::phase11
{
namespace
{

using Parameters = std::vector<std::optional<std::string>>;

class Result final
{
public:
    explicit Result(PGresult *value = nullptr) : value_(value) {}
    ~Result() { if (value_) PQclear(value_); }
    Result(const Result &) = delete;
    Result &operator=(const Result &) = delete;
    Result(Result &&other) noexcept : value_(std::exchange(other.value_, nullptr)) {}

    [[nodiscard]] int rows() const noexcept { return PQntuples(value_); }
    [[nodiscard]] bool isNull(int row, int column) const noexcept
    {
        return PQgetisnull(value_, row, column) != 0;
    }
    [[nodiscard]] std::string value(int row, int column) const
    {
        return isNull(row, column) ? std::string{} : PQgetvalue(value_, row, column);
    }

private:
    PGresult *value_ = nullptr;
};

Result execute(PGconn *connection, std::string_view sql,
               const Parameters &parameters, ExecStatusType expected)
{
    std::vector<const char *> values;
    values.reserve(parameters.size());
    for (const auto &parameter : parameters)
        values.push_back(parameter ? parameter->c_str() : nullptr);

    PGresult *raw = PQexecParams(
        connection, std::string(sql).c_str(), static_cast<int>(values.size()),
        nullptr, values.empty() ? nullptr : values.data(), nullptr, nullptr, 0);
    if (!raw)
        throw std::runtime_error("libpq returned no result: " +
                                 std::string(PQerrorMessage(connection)));
    if (PQresultStatus(raw) != expected)
    {
        const std::string error = PQresultErrorMessage(raw);
        PQclear(raw);
        throw std::runtime_error("PostgreSQL statement failed: " + error);
    }
    return Result(raw);
}

Result query(PGconn *connection, std::string_view sql,
             const Parameters &parameters = {})
{
    return execute(connection, sql, parameters, PGRES_TUPLES_OK);
}

void command(PGconn *connection, std::string_view sql,
             const Parameters &parameters = {})
{
    (void)execute(connection, sql, parameters, PGRES_COMMAND_OK);
}

std::string number(std::uint64_t value) { return std::to_string(value); }

std::string encodeTime(TimePoint value)
{
    return std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                              value.time_since_epoch()).count());
}

TimePoint decodeTime(const Result &result, int row, int column)
{
    return TimePoint{std::chrono::microseconds{
        std::stoll(result.value(row, column))}};
}

std::optional<TimePoint> decodeOptionalTime(const Result &result,
                                            int row, int column)
{
    return result.isNull(row, column)
               ? std::nullopt
               : std::optional<TimePoint>{decodeTime(result, row, column)};
}

std::optional<std::uint64_t> decodeOptionalId(const Result &result,
                                              int row, int column)
{
    return result.isNull(row, column)
               ? std::nullopt
               : std::optional<std::uint64_t>{
                     std::stoull(result.value(row, column))};
}

bool decodeBool(const Result &result, int row, int column)
{
    return result.value(row, column) == "t";
}

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
    return role == MemberRole::Owner ? "owner" : "member";
}

MemberRole memberRole(std::string_view role)
{
    if (role == "owner") return MemberRole::Owner;
    if (role == "member") return MemberRole::Member;
    throw std::runtime_error("unknown persisted member role");
}

std::string nodeKind(ModelNodeKind kind)
{
    return kind == ModelNodeKind::Root ? "root" : "specialist";
}

ModelNodeKind nodeKind(std::string_view kind)
{
    if (kind == "root") return ModelNodeKind::Root;
    if (kind == "specialist") return ModelNodeKind::Specialist;
    throw std::runtime_error("unknown persisted model node kind");
}

std::string versionState(ModelVersionState state)
{
    switch (state)
    {
    case ModelVersionState::Candidate: return "candidate";
    case ModelVersionState::Active: return "active";
    case ModelVersionState::Retired: return "retired";
    case ModelVersionState::Rejected: return "rejected";
    }
    throw std::logic_error("unknown model version state");
}

ModelVersionState versionState(std::string_view state)
{
    if (state == "candidate") return ModelVersionState::Candidate;
    if (state == "active") return ModelVersionState::Active;
    if (state == "retired") return ModelVersionState::Retired;
    if (state == "rejected") return ModelVersionState::Rejected;
    throw std::runtime_error("unknown persisted model version state");
}

std::string edgeKind(ModelEdgeKind kind)
{
    return kind == ModelEdgeKind::ParentControlsChild
               ? "parent_controls_child"
               : "peer_collaborates";
}

ModelEdgeKind edgeKind(std::string_view kind)
{
    if (kind == "parent_controls_child")
        return ModelEdgeKind::ParentControlsChild;
    if (kind == "peer_collaborates")
        return ModelEdgeKind::PeerCollaborates;
    throw std::runtime_error("unknown persisted model edge kind");
}

std::string trainingState(TrainingCandidateState state)
{
    switch (state)
    {
    case TrainingCandidateState::Submitted: return "submitted";
    case TrainingCandidateState::Approved: return "approved";
    case TrainingCandidateState::Rejected: return "rejected";
    case TrainingCandidateState::Exported: return "exported";
    case TrainingCandidateState::Revoked: return "revoked";
    }
    throw std::logic_error("unknown training state");
}

TrainingCandidateState trainingState(std::string_view state)
{
    if (state == "submitted") return TrainingCandidateState::Submitted;
    if (state == "approved") return TrainingCandidateState::Approved;
    if (state == "rejected") return TrainingCandidateState::Rejected;
    if (state == "exported") return TrainingCandidateState::Exported;
    if (state == "revoked") return TrainingCandidateState::Revoked;
    throw std::runtime_error("unknown persisted training state");
}

User readUser(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)), result.value(row, 1),
            result.value(row, 2), result.value(row, 3), result.value(row, 4),
            decodeBool(result, row, 5), decodeBool(result, row, 6),
            decodeTime(result, row, 7)};
}

Session readSession(const Result &result, int row)
{
    return {result.value(row, 0), std::stoull(result.value(row, 1)),
            result.value(row, 2), result.value(row, 3),
            decodeTime(result, row, 4), decodeTime(result, row, 5),
            decodeOptionalTime(result, row, 6)};
}

FriendRequest readFriendRequest(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)),
            std::stoull(result.value(row, 2)),
            friendState(result.value(row, 3)), decodeTime(result, row, 4),
            decodeOptionalTime(result, row, 5)};
}

Conversation readConversation(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            conversationKind(result.value(row, 1)), result.value(row, 2),
            std::stoull(result.value(row, 3)),
            std::stoull(result.value(row, 4)), decodeTime(result, row, 5)};
}

ConversationMember readMember(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)), memberRole(result.value(row, 2)),
            std::stoull(result.value(row, 3)),
            std::stoull(result.value(row, 4)), decodeTime(result, row, 5)};
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
    return message;
}

OutboxEvent readOutbox(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)), result.value(row, 1),
            result.value(row, 2), std::stoull(result.value(row, 3)),
            result.value(row, 4), decodeTime(result, row, 5),
            decodeOptionalTime(result, row, 6)};
}

ModelNode readNode(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)), result.value(row, 1),
            nodeKind(result.value(row, 2)), decodeOptionalId(result, row, 3),
            decodeTime(result, row, 4)};
}

ModelVersion readVersion(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)), result.value(row, 2),
            result.value(row, 3), result.value(row, 4), result.value(row, 5),
            versionState(result.value(row, 6)), decodeTime(result, row, 7)};
}

TrainingCandidate readCandidate(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)),
            std::stoull(result.value(row, 2)), result.value(row, 3),
            result.value(row, 4), result.value(row, 5),
            trainingState(result.value(row, 6)),
            decodeOptionalId(result, row, 7), decodeTime(result, row, 8)};
}

thread_local std::unordered_map<const PostgresStore *, PGconn *>
    activeTransactions;

constexpr std::string_view kUserColumns =
    "id, username, display_name, biography, password_hash, disabled, ai_account, "
    "(extract(epoch FROM created_at) * 1000000)::bigint";
constexpr std::string_view kSessionColumns =
    "id, user_id, token_hash, csrf_hash, "
    "(extract(epoch FROM created_at) * 1000000)::bigint, "
    "(extract(epoch FROM expires_at) * 1000000)::bigint, "
    "(extract(epoch FROM revoked_at) * 1000000)::bigint";
constexpr std::string_view kFriendColumns =
    "id, sender_id, receiver_id, state, "
    "(extract(epoch FROM created_at) * 1000000)::bigint, "
    "(extract(epoch FROM decided_at) * 1000000)::bigint";
constexpr std::string_view kConversationColumns =
    "id, kind, title, created_by, next_sequence, "
    "(extract(epoch FROM created_at) * 1000000)::bigint";
constexpr std::string_view kQualifiedConversationColumns =
    "c.id, c.kind, c.title, c.created_by, c.next_sequence, "
    "(extract(epoch FROM c.created_at) * 1000000)::bigint";
constexpr std::string_view kMemberColumns =
    "conversation_id, user_id, role, last_delivered_sequence, "
    "last_read_sequence, (extract(epoch FROM joined_at) * 1000000)::bigint";
constexpr std::string_view kMessageColumns =
    "id, conversation_id, conversation_seq, sender_id, client_message_id, body, "
    "model_node_id, model_version_id, adapter_name, "
    "(extract(epoch FROM created_at) * 1000000)::bigint";
constexpr std::string_view kOutboxColumns =
    "id, event_type, aggregate_type, aggregate_id, payload, "
    "(extract(epoch FROM created_at) * 1000000)::bigint, "
    "(extract(epoch FROM published_at) * 1000000)::bigint";
constexpr std::string_view kNodeColumns =
    "id, name, kind, active_version_id, "
    "(extract(epoch FROM created_at) * 1000000)::bigint";
constexpr std::string_view kVersionColumns =
    "id, node_id, runtime, model_artifact, adapter_artifact, checksum, state, "
    "(extract(epoch FROM created_at) * 1000000)::bigint";
constexpr std::string_view kCandidateColumns =
    "id, owner_user_id, source_message_id, sanitized_prompt, sanitized_response, "
    "consent_version, state, dataset_version_id, "
    "(extract(epoch FROM created_at) * 1000000)::bigint";

} // namespace

class PostgresStore::Transaction final : public ITransaction
{
public:
    Transaction(PostgresStore &store, PostgresConnectionPool::Lease lease)
        : store_(store), lease_(std::move(lease)), owner_(std::this_thread::get_id())
    {
        const auto [_, inserted] = activeTransactions.emplace(&store_, lease_.get());
        if (!inserted)
            throw std::logic_error("nested PostgreSQL transactions are not supported");
        try
        {
            command(lease_.get(), "BEGIN");
        }
        catch (...)
        {
            activeTransactions.erase(&store_);
            throw;
        }
    }

    ~Transaction() override
    {
        if (!finished_)
        {
            try { rollback(); }
            catch (...) { activeTransactions.erase(&store_); }
        }
    }

    void commit() override { finish("COMMIT"); }
    void rollback() override { finish("ROLLBACK"); }

private:
    void finish(std::string_view action)
    {
        if (finished_)
            throw std::logic_error("transaction already completed");
        if (owner_ != std::this_thread::get_id())
            throw std::logic_error("transaction completed on a different thread");
        try
        {
            command(lease_.get(), action);
        }
        catch (...)
        {
            activeTransactions.erase(&store_);
            finished_ = true;
            lease_.reset();
            throw;
        }
        activeTransactions.erase(&store_);
        finished_ = true;
        lease_.reset();
    }

    PostgresStore &store_;
    PostgresConnectionPool::Lease lease_;
    std::thread::id owner_;
    bool finished_ = false;
};

PostgresStore::PostgresStore(PostgresPoolConfig config)
    : pool_(std::move(config))
{
}

PostgresStore::ConnectionScope PostgresStore::connection() const
{
    if (const auto found = activeTransactions.find(this);
        found != activeTransactions.end())
        return {{}, found->second};
    ConnectionScope scope;
    scope.lease.emplace(pool_.acquire());
    scope.connection = scope.lease->get();
    return scope;
}

std::unique_ptr<ITransaction> PostgresStore::beginTransaction()
{
    return std::make_unique<Transaction>(*this, pool_.acquire());
}

bool PostgresStore::healthy() const noexcept
{
    try
    {
        auto scope = connection();
        return query(scope.connection, "SELECT 1").rows() == 1;
    }
    catch (...)
    {
        return false;
    }
}

User PostgresStore::create(User user)
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "INSERT INTO users(username, display_name, biography, password_hash, disabled, ai_account, created_at) "
        "VALUES($1,$2,$3,$4,$5::boolean,$6::boolean,to_timestamp($7::double precision/1000000.0)) RETURNING " +
        std::string(kUserColumns),
        {user.username, user.displayName, user.biography, user.passwordHash,
         user.disabled ? "true" : "false", user.aiAccount ? "true" : "false",
         encodeTime(user.createdAt)});
    return readUser(result, 0);
}

std::optional<User> PostgresStore::findUser(UserId id) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kUserColumns) + " FROM users WHERE id=$1::bigint",
        {number(id)});
    return result.rows() ? std::optional<User>{readUser(result, 0)} : std::nullopt;
}

std::optional<User> PostgresStore::findUserByName(std::string_view username) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kUserColumns) + " FROM users WHERE username=$1",
        {std::string(username)});
    return result.rows() ? std::optional<User>{readUser(result, 0)} : std::nullopt;
}

bool PostgresStore::updateProfile(UserId id, std::string displayName,
                                  std::string biography)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE users SET display_name=$2, biography=$3 WHERE id=$1::bigint RETURNING 1",
        {number(id), std::move(displayName), std::move(biography)}).rows() == 1;
}

Session PostgresStore::createSession(Session session)
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "INSERT INTO user_sessions(id,user_id,token_hash,csrf_hash,created_at,expires_at,revoked_at) "
        "VALUES($1,$2::bigint,$3,$4,to_timestamp($5::double precision/1000000.0),"
        "to_timestamp($6::double precision/1000000.0),NULL) RETURNING " +
        std::string(kSessionColumns),
        {session.id, number(session.userId), session.tokenHash, session.csrfHash,
         encodeTime(session.createdAt), encodeTime(session.expiresAt)});
    return readSession(result, 0);
}

std::optional<Session> PostgresStore::findSessionByTokenHash(
    std::string_view tokenHash) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kSessionColumns) +
        " FROM user_sessions WHERE token_hash=$1", {std::string(tokenHash)});
    return result.rows() ? std::optional<Session>{readSession(result, 0)}
                         : std::nullopt;
}

bool PostgresStore::revokeSession(std::string_view sessionId, TimePoint now)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE user_sessions SET revoked_at=to_timestamp($2::double precision/1000000.0) "
        "WHERE id=$1 RETURNING 1", {std::string(sessionId), encodeTime(now)}).rows() == 1;
}

FriendRequest PostgresStore::createFriendRequest(FriendRequest request)
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "INSERT INTO friend_requests(sender_id,receiver_id,state,created_at) "
        "VALUES($1::bigint,$2::bigint,$3,to_timestamp($4::double precision/1000000.0)) RETURNING " +
        std::string(kFriendColumns),
        {number(request.senderId), number(request.receiverId),
         friendState(request.state), encodeTime(request.createdAt)});
    return readFriendRequest(result, 0);
}

std::optional<FriendRequest> PostgresStore::findFriendRequest(
    FriendRequestId id) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kFriendColumns) +
        " FROM friend_requests WHERE id=$1::bigint", {number(id)});
    return result.rows() ? std::optional<FriendRequest>{readFriendRequest(result, 0)}
                         : std::nullopt;
}

bool PostgresStore::decideFriendRequest(FriendRequestId id, UserId receiver,
                                        FriendRequestState decision, TimePoint now)
{
    auto scope = connection();
    const auto result = query(scope.connection,
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

bool PostgresStore::areFriends(UserId first, UserId second) const
{
    auto scope = connection();
    return query(scope.connection,
        "SELECT 1 FROM friendships WHERE user_low_id=LEAST($1::bigint,$2::bigint) "
        "AND user_high_id=GREATEST($1::bigint,$2::bigint)",
        {number(first), number(second)}).rows() == 1;
}

std::vector<UserId> PostgresStore::listFriendIds(UserId user) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT CASE WHEN user_low_id=$1::bigint THEN user_high_id ELSE user_low_id END "
        "FROM friendships WHERE user_low_id=$1::bigint OR user_high_id=$1::bigint "
        "ORDER BY 1", {number(user)});
    std::vector<UserId> friends;
    friends.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        friends.push_back(std::stoull(result.value(row, 0)));
    return friends;
}

std::vector<FriendRequest> PostgresStore::pendingRequests(UserId receiver) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kFriendColumns) +
        " FROM friend_requests WHERE receiver_id=$1::bigint AND state='pending' ORDER BY id",
        {number(receiver)});
    std::vector<FriendRequest> requests;
    requests.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        requests.push_back(readFriendRequest(result, row));
    return requests;
}

Conversation PostgresStore::createConversation(
    Conversation conversation, std::vector<ConversationMember> members)
{
    auto scope = connection();
    const bool localTransaction = scope.lease.has_value();
    if (localTransaction) command(scope.connection, "BEGIN");
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
        const auto result = query(scope.connection,
            "INSERT INTO conversations(kind,title,created_by,direct_pair_key,created_at) "
            "VALUES($1,$2,$3::bigint,$4,to_timestamp($5::double precision/1000000.0)) RETURNING " +
            std::string(kConversationColumns),
            {conversationKind(conversation.kind), conversation.title,
             number(conversation.createdBy), directKey, encodeTime(conversation.createdAt)});
        auto created = readConversation(result, 0);
        for (const auto &member : members)
            (void)query(scope.connection,
                "INSERT INTO conversation_members(conversation_id,user_id,role,last_delivered_sequence,last_read_sequence,joined_at) "
                "VALUES($1::bigint,$2::bigint,$3,$4::bigint,$5::bigint,to_timestamp($6::double precision/1000000.0)) RETURNING 1",
                {number(created.id), number(member.userId), memberRole(member.role),
                 number(member.lastDeliveredSequence), number(member.lastReadSequence),
                 encodeTime(member.joinedAt)});
        if (localTransaction) command(scope.connection, "COMMIT");
        return created;
    }
    catch (...)
    {
        if (localTransaction)
        {
            try { command(scope.connection, "ROLLBACK"); } catch (...) {}
        }
        throw;
    }
}

std::optional<Conversation> PostgresStore::findConversation(ConversationId id) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kConversationColumns) +
        " FROM conversations WHERE id=$1::bigint", {number(id)});
    return result.rows() ? std::optional<Conversation>{readConversation(result, 0)}
                         : std::nullopt;
}

std::optional<ConversationMember> PostgresStore::findMember(
    ConversationId conversation, UserId user) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kMemberColumns) +
        " FROM conversation_members WHERE conversation_id=$1::bigint AND user_id=$2::bigint",
        {number(conversation), number(user)});
    return result.rows()
               ? std::optional<ConversationMember>{readMember(result, 0)}
               : std::nullopt;
}

std::optional<Conversation> PostgresStore::findDirectConversation(
    UserId first, UserId second) const
{
    auto scope = connection();
    const auto key = number(std::min(first, second)) + ":" +
                     number(std::max(first, second));
    const auto result = query(scope.connection,
        "SELECT " + std::string(kConversationColumns) +
        " FROM conversations WHERE direct_pair_key=$1", {key});
    return result.rows() ? std::optional<Conversation>{readConversation(result, 0)}
                         : std::nullopt;
}

std::vector<Conversation> PostgresStore::listConversations(UserId user) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kQualifiedConversationColumns) +
        " FROM conversations c JOIN conversation_members m ON m.conversation_id=c.id "
        "WHERE m.user_id=$1::bigint ORDER BY c.id", {number(user)});
    std::vector<Conversation> conversations;
    conversations.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        conversations.push_back(readConversation(result, row));
    return conversations;
}

std::vector<ConversationMember> PostgresStore::listMembers(
    ConversationId conversation) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT conversation_id,user_id,role,last_delivered_sequence,last_read_sequence,"
        "(extract(epoch from joined_at)*1000000)::bigint "
        "FROM conversation_members WHERE conversation_id=$1::bigint ORDER BY user_id",
        {number(conversation)});
    std::vector<ConversationMember> members;
    members.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        members.push_back(readMember(result, row));
    return members;
}

bool PostgresStore::addMember(ConversationMember member)
{
    auto scope = connection();
    return query(scope.connection,
        "INSERT INTO conversation_members(conversation_id,user_id,role,last_delivered_sequence,last_read_sequence,joined_at) "
        "VALUES($1::bigint,$2::bigint,$3,$4::bigint,$5::bigint,to_timestamp($6::double precision/1000000.0)) "
        "ON CONFLICT DO NOTHING RETURNING 1",
        {number(member.conversationId), number(member.userId), memberRole(member.role),
         number(member.lastDeliveredSequence), number(member.lastReadSequence),
         encodeTime(member.joinedAt)}).rows() == 1;
}

bool PostgresStore::updateDelivered(ConversationId conversation, UserId user,
                                    std::uint64_t sequence)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE conversation_members SET last_delivered_sequence=GREATEST(last_delivered_sequence,$3::bigint) "
        "WHERE conversation_id=$1::bigint AND user_id=$2::bigint RETURNING 1",
        {number(conversation), number(user), number(sequence)}).rows() == 1;
}

bool PostgresStore::updateRead(ConversationId conversation, UserId user,
                               std::uint64_t sequence)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE conversation_members SET last_read_sequence=GREATEST(last_read_sequence,$3::bigint), "
        "last_delivered_sequence=GREATEST(last_delivered_sequence,$3::bigint) "
        "WHERE conversation_id=$1::bigint AND user_id=$2::bigint RETURNING 1",
        {number(conversation), number(user), number(sequence)}).rows() == 1;
}

AppendMessageResult PostgresStore::appendMessage(Message message)
{
    auto scope = connection();
    const bool localTransaction = scope.lease.has_value();
    if (localTransaction) command(scope.connection, "BEGIN");
    try
    {
        (void)query(scope.connection, "SELECT pg_advisory_xact_lock($1::bigint)",
                    {number(message.conversationId)});
        auto existing = query(scope.connection,
            "SELECT " + std::string(kMessageColumns) +
            " FROM messages WHERE conversation_id=$1::bigint AND sender_id=$2::bigint AND client_message_id=$3",
            {number(message.conversationId), number(message.senderId),
             message.clientMessageId});
        if (existing.rows())
        {
            if (localTransaction) command(scope.connection, "COMMIT");
            return {readMessage(existing, 0), false};
        }

        const auto allocated = query(scope.connection,
            "UPDATE conversations SET next_sequence=next_sequence+1 WHERE id=$1::bigint "
            "RETURNING next_sequence-1", {number(message.conversationId)});
        if (!allocated.rows())
            throw std::invalid_argument("conversation does not exist");
        const auto sequence = allocated.value(0, 0);
        const auto inserted = query(scope.connection,
            "INSERT INTO messages(conversation_id,conversation_seq,sender_id,client_message_id,body,model_node_id,model_version_id,adapter_name,created_at) "
            "VALUES($1::bigint,$2::bigint,$3::bigint,$4,$5,$6::bigint,$7::bigint,$8,to_timestamp($9::double precision/1000000.0)) RETURNING " +
            std::string(kMessageColumns),
            {number(message.conversationId), sequence, number(message.senderId),
             message.clientMessageId, message.body,
             message.modelTrace.nodeId ? std::optional<std::string>{number(*message.modelTrace.nodeId)} : std::nullopt,
             message.modelTrace.versionId ? std::optional<std::string>{number(*message.modelTrace.versionId)} : std::nullopt,
             message.modelTrace.adapterName, encodeTime(message.createdAt)});
        if (localTransaction) command(scope.connection, "COMMIT");
        return {readMessage(inserted, 0), true};
    }
    catch (...)
    {
        if (localTransaction)
        {
            try { command(scope.connection, "ROLLBACK"); } catch (...) {}
        }
        throw;
    }
}

std::vector<Message> PostgresStore::messagesAfter(
    ConversationId conversation, std::uint64_t sequence,
    std::size_t limit) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kMessageColumns) +
        " FROM messages WHERE conversation_id=$1::bigint AND conversation_seq>$2::bigint "
        "ORDER BY conversation_seq LIMIT $3::bigint",
        {number(conversation), number(sequence), number(limit)});
    std::vector<Message> messages;
    messages.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        messages.push_back(readMessage(result, row));
    return messages;
}

std::vector<Message> PostgresStore::searchMessages(
    ConversationId conversation, std::string_view text,
    std::size_t limit) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kMessageColumns) +
        " FROM messages WHERE conversation_id=$1::bigint AND body ILIKE '%' || $2 || '%' "
        "ORDER BY conversation_seq LIMIT $3::bigint",
        {number(conversation), std::string(text), number(limit)});
    std::vector<Message> messages;
    messages.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        messages.push_back(readMessage(result, row));
    return messages;
}

std::optional<Message> PostgresStore::findMessage(MessageId id) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kMessageColumns) +
        " FROM messages WHERE id=$1::bigint", {number(id)});
    return result.rows() ? std::optional<Message>{readMessage(result, 0)}
                         : std::nullopt;
}

OutboxEvent PostgresStore::appendOutbox(OutboxEvent event)
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "INSERT INTO outbox_events(event_type,aggregate_type,aggregate_id,payload,created_at) "
        "VALUES($1,$2,$3::bigint,$4,to_timestamp($5::double precision/1000000.0)) RETURNING " +
        std::string(kOutboxColumns),
        {event.type, event.aggregateType, number(event.aggregateId), event.payload,
         encodeTime(event.createdAt)});
    return readOutbox(result, 0);
}

std::vector<OutboxEvent> PostgresStore::pendingOutbox(std::size_t limit) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kOutboxColumns) +
        " FROM outbox_events WHERE published_at IS NULL ORDER BY id LIMIT $1::bigint",
        {number(limit)});
    std::vector<OutboxEvent> events;
    events.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        events.push_back(readOutbox(result, row));
    return events;
}

bool PostgresStore::markPublished(OutboxEventId id, TimePoint now)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE outbox_events SET published_at=to_timestamp($2::double precision/1000000.0) "
        "WHERE id=$1::bigint RETURNING 1", {number(id), encodeTime(now)}).rows() == 1;
}

ModelNode PostgresStore::createModelNode(ModelNode node)
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "INSERT INTO model_nodes(name,kind,created_at) VALUES($1,$2,to_timestamp($3::double precision/1000000.0)) RETURNING " +
        std::string(kNodeColumns), {node.name, nodeKind(node.kind), encodeTime(node.createdAt)});
    return readNode(result, 0);
}

ModelVersion PostgresStore::createModelVersion(ModelVersion version)
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "INSERT INTO model_versions(node_id,runtime,model_artifact,adapter_artifact,checksum,state,created_at) "
        "VALUES($1::bigint,$2,$3,$4,$5,$6,to_timestamp($7::double precision/1000000.0)) RETURNING " +
        std::string(kVersionColumns),
        {number(version.nodeId), version.runtime, version.modelArtifact,
         version.adapterArtifact, version.checksum, versionState(version.state),
         encodeTime(version.createdAt)});
    return readVersion(result, 0);
}

ModelEdge PostgresStore::createModelEdge(ModelEdge edge)
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "INSERT INTO model_edges(from_node_id,to_node_id,kind) VALUES($1::bigint,$2::bigint,$3) "
        "RETURNING id,from_node_id,to_node_id,kind",
        {number(edge.from), number(edge.to), edgeKind(edge.kind)});
    return {std::stoull(result.value(0, 0)), std::stoull(result.value(0, 1)),
            std::stoull(result.value(0, 2)), edgeKind(result.value(0, 3))};
}

bool PostgresStore::bindAiAccount(ModelBinding binding)
{
    auto scope = connection();
    const auto user = query(scope.connection,
        "SELECT 1 FROM users WHERE id=$1::bigint AND ai_account=true",
        {number(binding.aiUserId)});
    if (!user.rows()) return false;
    return query(scope.connection,
        "INSERT INTO model_bindings(ai_user_id,model_node_id) VALUES($1::bigint,$2::bigint) "
        "ON CONFLICT(ai_user_id) DO UPDATE SET model_node_id=EXCLUDED.model_node_id RETURNING 1",
        {number(binding.aiUserId), number(binding.nodeId)}).rows() == 1;
}

std::optional<ModelNode> PostgresStore::findModelNode(ModelNodeId id) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kNodeColumns) + " FROM model_nodes WHERE id=$1::bigint",
        {number(id)});
    return result.rows() ? std::optional<ModelNode>{readNode(result, 0)} : std::nullopt;
}

std::optional<ModelVersion> PostgresStore::findModelVersion(ModelVersionId id) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kVersionColumns) + " FROM model_versions WHERE id=$1::bigint",
        {number(id)});
    return result.rows() ? std::optional<ModelVersion>{readVersion(result, 0)}
                         : std::nullopt;
}

std::optional<ModelBinding> PostgresStore::findModelBinding(UserId aiUser) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT ai_user_id,model_node_id FROM model_bindings WHERE ai_user_id=$1::bigint",
        {number(aiUser)});
    return result.rows()
               ? std::optional<ModelBinding>{{std::stoull(result.value(0, 0)),
                                               std::stoull(result.value(0, 1))}}
               : std::nullopt;
}

std::vector<ModelEdge> PostgresStore::modelEdges() const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT id,from_node_id,to_node_id,kind FROM model_edges ORDER BY id");
    std::vector<ModelEdge> edges;
    edges.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        edges.push_back({std::stoull(result.value(row, 0)),
                         std::stoull(result.value(row, 1)),
                         std::stoull(result.value(row, 2)),
                         edgeKind(result.value(row, 3))});
    return edges;
}

bool PostgresStore::activateModelVersion(ModelNodeId node, ModelVersionId version)
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "WITH locked_node AS ("
        " SELECT active_version_id FROM model_nodes WHERE id=$1::bigint FOR UPDATE"
        "), target AS ("
        " SELECT id FROM model_versions WHERE id=$2::bigint AND node_id=$1::bigint"
        " AND checksum<>'' AND state<>'rejected'"
        "), retired AS ("
        " UPDATE model_versions SET state='retired' WHERE id=(SELECT active_version_id FROM locked_node)"
        " AND id<>$2::bigint AND EXISTS(SELECT 1 FROM target)"
        "), activated AS ("
        " UPDATE model_versions SET state='active' WHERE id=(SELECT id FROM target) RETURNING id"
        ") UPDATE model_nodes SET active_version_id=(SELECT id FROM activated)"
        " WHERE id=$1::bigint AND EXISTS(SELECT 1 FROM activated) RETURNING 1",
        {number(node), number(version)});
    return result.rows() == 1;
}

TrainingCandidate PostgresStore::createTrainingCandidate(
    TrainingCandidate candidate)
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "INSERT INTO training_candidates(owner_user_id,source_message_id,sanitized_prompt,sanitized_response,consent_version,state,dataset_version_id,created_at) "
        "VALUES($1::bigint,$2::bigint,$3,$4,$5,$6,NULL,to_timestamp($7::double precision/1000000.0)) RETURNING " +
        std::string(kCandidateColumns),
        {number(candidate.ownerUserId), number(candidate.sourceMessageId),
         candidate.sanitizedPrompt, candidate.sanitizedResponse,
         candidate.consentVersion, trainingState(candidate.state),
         encodeTime(candidate.createdAt)});
    return readCandidate(result, 0);
}

std::optional<TrainingCandidate> PostgresStore::findTrainingCandidate(
    TrainingCandidateId id) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kCandidateColumns) +
        " FROM training_candidates WHERE id=$1::bigint", {number(id)});
    return result.rows()
               ? std::optional<TrainingCandidate>{readCandidate(result, 0)}
               : std::nullopt;
}

bool PostgresStore::updateTrainingCandidate(TrainingCandidate candidate)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE training_candidates SET sanitized_prompt=$2,sanitized_response=$3,consent_version=$4,state=$5,dataset_version_id=$6::bigint "
        "WHERE id=$1::bigint RETURNING 1",
        {number(candidate.id), candidate.sanitizedPrompt, candidate.sanitizedResponse,
         candidate.consentVersion, trainingState(candidate.state),
         candidate.datasetVersionId ? std::optional<std::string>{number(*candidate.datasetVersionId)} : std::nullopt}).rows() == 1;
}

DatasetVersion PostgresStore::createDatasetVersion(DatasetVersion version)
{
    auto scope = connection();
    const bool localTransaction = scope.lease.has_value();
    if (localTransaction) command(scope.connection, "BEGIN");
    try
    {
        const auto result = query(scope.connection,
            "INSERT INTO dataset_versions(name,checksum,created_at) "
            "VALUES($1,$2,to_timestamp($3::double precision/1000000.0)) RETURNING id,name,checksum,(extract(epoch FROM created_at)*1000000)::bigint",
            {version.name, version.checksum, encodeTime(version.createdAt)});
        version.id = std::stoull(result.value(0, 0));
        version.name = result.value(0, 1);
        version.checksum = result.value(0, 2);
        version.createdAt = decodeTime(result, 0, 3);
        for (const auto candidate : version.candidates)
            (void)query(scope.connection,
                "INSERT INTO dataset_candidates(dataset_version_id,candidate_id) VALUES($1::bigint,$2::bigint) RETURNING 1",
                {number(version.id), number(candidate)});
        if (localTransaction) command(scope.connection, "COMMIT");
        return version;
    }
    catch (...)
    {
        if (localTransaction)
        {
            try { command(scope.connection, "ROLLBACK"); } catch (...) {}
        }
        throw;
    }
}

} // namespace webserver::phase11
