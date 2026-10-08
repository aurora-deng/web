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

std::optional<std::string> encodeOptionalTime(
    const std::optional<TimePoint> &value)
{
    return value ? std::optional<std::string>{encodeTime(*value)}
                 : std::nullopt;
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

MessageState messageState(std::string_view state)
{
    if (state == "active") return MessageState::Active;
    if (state == "recalled") return MessageState::Recalled;
    throw std::runtime_error("unknown persisted message state");
}

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

std::string knowledgeScope(KnowledgeScope scope)
{
    return scope == KnowledgeScope::PrivateUser ? "private" : "conversation";
}

KnowledgeScope knowledgeScope(std::string_view scope)
{
    if (scope == "private") return KnowledgeScope::PrivateUser;
    if (scope == "conversation") return KnowledgeScope::Conversation;
    throw std::runtime_error("unknown persisted knowledge scope");
}

std::string knowledgeState(KnowledgeDocumentState state)
{
    switch (state)
    {
    case KnowledgeDocumentState::Indexing: return "indexing";
    case KnowledgeDocumentState::Ready: return "ready";
    case KnowledgeDocumentState::Failed: return "failed";
    case KnowledgeDocumentState::Deleted: return "deleted";
    }
    throw std::logic_error("unknown knowledge document state");
}

KnowledgeDocumentState knowledgeState(std::string_view state)
{
    if (state == "indexing") return KnowledgeDocumentState::Indexing;
    if (state == "ready") return KnowledgeDocumentState::Ready;
    if (state == "failed") return KnowledgeDocumentState::Failed;
    if (state == "deleted") return KnowledgeDocumentState::Deleted;
    throw std::runtime_error("unknown persisted knowledge document state");
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

std::string activationAction(ModelActivationAction action)
{
    return action == ModelActivationAction::Activate ? "activate" : "rollback";
}

ModelActivationAction activationAction(std::string_view action)
{
    if (action == "activate") return ModelActivationAction::Activate;
    if (action == "rollback") return ModelActivationAction::Rollback;
    throw std::runtime_error("unknown persisted model activation action");
}

std::string activationOutcome(ModelActivationOutcome outcome)
{
    return outcome == ModelActivationOutcome::Succeeded ? "succeeded" : "failed";
}

ModelActivationOutcome activationOutcome(std::string_view outcome)
{
    if (outcome == "succeeded") return ModelActivationOutcome::Succeeded;
    if (outcome == "failed") return ModelActivationOutcome::Failed;
    throw std::runtime_error("unknown persisted model activation outcome");
}

std::string generationStatus(AiGenerationStatus status)
{
    switch (status)
    {
    case AiGenerationStatus::Queued: return "queued";
    case AiGenerationStatus::Running: return "running";
    case AiGenerationStatus::Succeeded: return "succeeded";
    case AiGenerationStatus::Failed: return "failed";
    case AiGenerationStatus::Cancelled: return "cancelled";
    case AiGenerationStatus::TimedOut: return "timed_out";
    }
    throw std::logic_error("unknown AI generation status");
}

AiGenerationStatus generationStatus(std::string_view status)
{
    if (status == "queued") return AiGenerationStatus::Queued;
    if (status == "running") return AiGenerationStatus::Running;
    if (status == "succeeded") return AiGenerationStatus::Succeeded;
    if (status == "failed") return AiGenerationStatus::Failed;
    if (status == "cancelled") return AiGenerationStatus::Cancelled;
    if (status == "timed_out") return AiGenerationStatus::TimedOut;
    throw std::runtime_error("unknown persisted AI generation status");
}

std::string evaluationRunStatus(ModelEvaluationRunStatus status)
{
    switch (status)
    {
    case ModelEvaluationRunStatus::Running: return "running";
    case ModelEvaluationRunStatus::Completed: return "completed";
    case ModelEvaluationRunStatus::Aborted: return "aborted";
    }
    throw std::logic_error("unknown model evaluation run status");
}

ModelEvaluationRunStatus evaluationRunStatus(std::string_view status)
{
    if (status == "running") return ModelEvaluationRunStatus::Running;
    if (status == "completed") return ModelEvaluationRunStatus::Completed;
    if (status == "aborted") return ModelEvaluationRunStatus::Aborted;
    throw std::runtime_error("unknown persisted model evaluation run status");
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
            decodeTime(result, row, 7), decodeOptionalId(result, row, 8)};
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

FileObject readFile(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)), result.value(row, 2),
            result.value(row, 3), std::stoull(result.value(row, 4)),
            std::stoull(result.value(row, 5)), result.value(row, 6),
            fileState(result.value(row, 7)), decodeTime(result, row, 8),
            decodeOptionalTime(result, row, 9)};
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

ModelActivationAudit readActivationAudit(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)),
            decodeOptionalId(result, row, 2),
            std::stoull(result.value(row, 3)),
            activationAction(result.value(row, 4)),
            activationOutcome(result.value(row, 5)),
            result.value(row, 6), result.value(row, 7),
            decodeTime(result, row, 8)};
}

AiGenerationAudit readGenerationAudit(const Result &result, int row)
{
    AiGenerationAudit audit;
    audit.generationId = result.value(row, 0);
    audit.requesterId = std::stoull(result.value(row, 1));
    audit.aiUserId = std::stoull(result.value(row, 2));
    audit.conversationId = std::stoull(result.value(row, 3));
    audit.modelNodeId = decodeOptionalId(result, row, 4);
    audit.modelVersionId = decodeOptionalId(result, row, 5);
    audit.runtime = result.value(row, 6);
    audit.modelArtifact = result.value(row, 7);
    audit.adapterArtifact = result.value(row, 8);
    audit.modelChecksum = result.value(row, 9);
    audit.promptSha256 = result.value(row, 10);
    audit.promptBytes = std::stoull(result.value(row, 11));
    audit.outputSha256 = result.value(row, 12);
    audit.outputBytes = std::stoull(result.value(row, 13));
    audit.outputTokens = std::stoull(result.value(row, 14));
    audit.status = generationStatus(result.value(row, 15));
    audit.errorDetail = result.value(row, 16);
    audit.requestedAt = decodeTime(result, row, 17);
    audit.startedAt = decodeOptionalTime(result, row, 18);
    audit.firstTokenAt = decodeOptionalTime(result, row, 19);
    audit.completedAt = decodeOptionalTime(result, row, 20);
    audit.responseMessageId = decodeOptionalId(result, row, 21);
    audit.revision = std::stoull(result.value(row, 22));
    return audit;
}

ModelEvaluationSuite readEvaluationSuite(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)), result.value(row, 1),
            result.value(row, 2), result.value(row, 3), result.value(row, 4),
            decodeTime(result, row, 5)};
}

ModelEvaluationCase readEvaluationCase(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)), result.value(row, 2),
            result.value(row, 3), result.value(row, 4),
            std::stod(result.value(row, 5)), decodeBool(result, row, 6)};
}

ModelEvaluationRun readEvaluationRun(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)),
            std::stoull(result.value(row, 2)),
            evaluationRunStatus(result.value(row, 3)),
            std::stoull(result.value(row, 4)),
            std::stoull(result.value(row, 5)),
            std::stoull(result.value(row, 6)),
            std::stod(result.value(row, 7)), decodeBool(result, row, 8),
            result.value(row, 9), decodeTime(result, row, 10),
            decodeOptionalTime(result, row, 11)};
}

ModelEvaluationCaseResult readEvaluationCaseResult(
    const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)),
            std::stod(result.value(row, 2)), decodeBool(result, row, 3),
            result.value(row, 4), std::stoull(result.value(row, 5)),
            result.value(row, 6)};
}

ModelActivationPolicy readActivationPolicy(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)),
            std::stod(result.value(row, 2)),
            std::stod(result.value(row, 3)),
            std::stoull(result.value(row, 4)), decodeTime(result, row, 5)};
}

KnowledgeDocument readKnowledgeDocument(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)),
            knowledgeScope(result.value(row, 2)),
            decodeOptionalId(result, row, 3), result.value(row, 4),
            result.value(row, 5), result.value(row, 6), result.value(row, 7),
            knowledgeState(result.value(row, 8)), result.value(row, 9),
            decodeTime(result, row, 10), decodeOptionalTime(result, row, 11)};
}

KnowledgeChunk readKnowledgeChunk(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)),
            std::stoull(result.value(row, 1)),
            std::stoull(result.value(row, 2)), result.value(row, 3),
            result.value(row, 4)};
}

RagRetrieval readRagRetrieval(const Result &result, int row)
{
    return {result.value(row, 0), std::stoull(result.value(row, 1)),
            std::stoull(result.value(row, 2)), result.value(row, 3),
            decodeTime(result, row, 4)};
}

RagCitation readRagCitation(const Result &result, int row)
{
    return {result.value(row, 0), std::stoull(result.value(row, 1)),
            std::stoull(result.value(row, 2)),
            std::stoull(result.value(row, 3)),
            std::stod(result.value(row, 4))};
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

DatasetVersion readDatasetVersion(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)), result.value(row, 1),
            result.value(row, 2), {}, decodeTime(result, row, 3),
            result.value(row, 4), std::stoull(result.value(row, 5))};
}

thread_local std::unordered_map<const PostgresStore *, PGconn *>
    activeTransactions;

constexpr std::string_view kUserColumns =
    "id, username, display_name, biography, password_hash, disabled, ai_account, "
    "(extract(epoch FROM created_at) * 1000000)::bigint, avatar_file_id";
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
constexpr std::string_view kMessageColumns =
    "id, conversation_id, conversation_seq, sender_id, client_message_id, body, "
    "model_node_id, model_version_id, adapter_name, "
    "(extract(epoch FROM created_at) * 1000000)::bigint, state, recalled_by, "
    "(extract(epoch FROM recalled_at) * 1000000)::bigint";
constexpr std::string_view kFileColumns =
    "id, owner_id, original_name, media_type, expected_size, uploaded_size, "
    "sha256, state, (extract(epoch FROM created_at) * 1000000)::bigint, "
    "(extract(epoch FROM ready_at) * 1000000)::bigint";
constexpr std::string_view kOutboxColumns =
    "id, event_type, aggregate_type, aggregate_id, payload, "
    "(extract(epoch FROM created_at) * 1000000)::bigint, "
    "(extract(epoch FROM published_at) * 1000000)::bigint";

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
constexpr std::string_view kNodeColumns =
    "id, name, kind, active_version_id, "
    "(extract(epoch FROM created_at) * 1000000)::bigint";
constexpr std::string_view kVersionColumns =
    "id, node_id, runtime, model_artifact, adapter_artifact, checksum, state, "
    "(extract(epoch FROM created_at) * 1000000)::bigint";
constexpr std::string_view kActivationAuditColumns =
    "id,node_id,previous_version_id,target_version_id,action,outcome,actor,detail,"
    "(extract(epoch FROM created_at) * 1000000)::bigint";
constexpr std::string_view kGenerationAuditColumns =
    "generation_id,requester_id,ai_user_id,conversation_id,model_node_id,"
    "model_version_id,runtime,model_artifact,adapter_artifact,model_checksum,"
    "prompt_sha256,prompt_bytes,output_sha256,output_bytes,output_tokens,status,"
    "error_detail,(extract(epoch FROM requested_at)*1000000)::bigint,"
    "(extract(epoch FROM started_at)*1000000)::bigint,"
    "(extract(epoch FROM first_token_at)*1000000)::bigint,"
    "(extract(epoch FROM completed_at)*1000000)::bigint,response_message_id,revision";
constexpr std::string_view kEvaluationSuiteColumns =
    "id,name,version,checksum,description,"
    "(extract(epoch FROM created_at)*1000000)::bigint";
constexpr std::string_view kEvaluationCaseColumns =
    "id,suite_id,name,prompt,expected_answer,weight::text,required";
constexpr std::string_view kEvaluationRunColumns =
    "id,suite_id,model_version_id,status,total_cases,evaluated_cases,passed_cases,"
    "weighted_score::text,required_cases_passed,detail,"
    "(extract(epoch FROM started_at)*1000000)::bigint,"
    "(extract(epoch FROM completed_at)*1000000)::bigint";
constexpr std::string_view kEvaluationResultColumns =
    "run_id,case_id,score::text,passed,output_sha256,latency_ms,detail";
constexpr std::string_view kActivationPolicyColumns =
    "node_id,suite_id,minimum_pass_rate::text,minimum_weighted_score::text,"
    "max_run_age_seconds,(extract(epoch FROM updated_at)*1000000)::bigint";
constexpr std::string_view kKnowledgeDocumentColumns =
    "id,owner_id,scope,conversation_id,title,source_name,media_type,sha256,state,"
    "failure_detail,(extract(epoch FROM created_at)*1000000)::bigint,"
    "(extract(epoch FROM indexed_at)*1000000)::bigint";
constexpr std::string_view kKnowledgeChunkColumns =
    "id,document_id,ordinal,content,sha256";
constexpr std::string_view kRagRetrievalColumns =
    "generation_id,requester_id,conversation_id,query_sha256,"
    "(extract(epoch FROM created_at)*1000000)::bigint";
constexpr std::string_view kRagCitationColumns =
    "generation_id,rank,document_id,chunk_id,score::text";
constexpr std::string_view kCandidateColumns =
    "id, owner_user_id, source_message_id, sanitized_prompt, sanitized_response, "
    "consent_version, state, dataset_version_id, "
    "(extract(epoch FROM created_at) * 1000000)::bigint";
constexpr std::string_view kDatasetVersionColumns =
    "id,name,checksum,(extract(epoch FROM created_at)*1000000)::bigint,"
    "artifact_path,record_count";

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

bool PostgresStore::updateAvatar(UserId id, std::optional<FileId> avatar)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE users SET avatar_file_id=$2::bigint WHERE id=$1::bigint "
        "AND ($2::bigint IS NULL OR EXISTS(SELECT 1 FROM file_objects f "
        "WHERE f.id=$2::bigint AND f.owner_id=$1::bigint AND f.state='ready')) "
        "RETURNING 1",
        {number(id), avatar ? std::optional<std::string>{number(*avatar)}
                            : std::nullopt}).rows() == 1;
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

bool PostgresStore::removeFriendship(UserId first, UserId second)
{
    auto scope = connection();
    const auto result = query(scope.connection,
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

ContactSettings PostgresStore::contactSettings(UserId owner,
                                                UserId contact) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT remark,notifications_muted FROM contact_preferences "
        "WHERE owner_id=$1::bigint AND contact_id=$2::bigint",
        {number(owner), number(contact)});
    if (!result.rows())
        return {owner, contact, {}, false};
    return {owner, contact, result.value(0, 0), decodeBool(result, 0, 1)};
}

bool PostgresStore::updateContactSettings(UserId owner, UserId contact,
                                          std::string remark,
                                          bool notificationsMuted)
{
    auto scope = connection();
    return query(scope.connection,
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

bool PostgresStore::blockUser(UserId blocker, UserId blocked, TimePoint now)
{
    auto scope = connection();
    const auto result = query(scope.connection,
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

bool PostgresStore::unblockUser(UserId blocker, UserId blocked)
{
    auto scope = connection();
    return query(scope.connection,
        "DELETE FROM user_blocks WHERE blocker_id=$1::bigint "
        "AND blocked_id=$2::bigint RETURNING 1",
        {number(blocker), number(blocked)}).rows() == 1;
}

bool PostgresStore::isBlocked(UserId blocker, UserId blocked) const
{
    auto scope = connection();
    return query(scope.connection,
        "SELECT 1 FROM user_blocks WHERE blocker_id=$1::bigint "
        "AND blocked_id=$2::bigint",
        {number(blocker), number(blocked)}).rows() == 1;
}

bool PostgresStore::blockedEitherDirection(UserId first, UserId second) const
{
    auto scope = connection();
    return query(scope.connection,
        "SELECT 1 FROM user_blocks WHERE "
        "(blocker_id=$1::bigint AND blocked_id=$2::bigint) OR "
        "(blocker_id=$2::bigint AND blocked_id=$1::bigint) LIMIT 1",
        {number(first), number(second)}).rows() == 1;
}

std::vector<UserId> PostgresStore::listBlockedIds(UserId blocker) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT blocked_id FROM user_blocks WHERE blocker_id=$1::bigint "
        "ORDER BY blocked_id", {number(blocker)});
    std::vector<UserId> blocked;
    blocked.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        blocked.push_back(std::stoull(result.value(row, 0)));
    return blocked;
}

bool PostgresStore::updateLastActive(UserId user, TimePoint now)
{
    auto scope = connection();
    return query(scope.connection,
        "INSERT INTO user_presence(user_id,last_active_at) "
        "SELECT $1::bigint,to_timestamp($2::double precision/1000000.0) "
        "WHERE EXISTS (SELECT 1 FROM users WHERE id=$1::bigint) "
        "ON CONFLICT(user_id) DO UPDATE SET last_active_at=EXCLUDED.last_active_at "
        "RETURNING 1", {number(user), encodeTime(now)}).rows() == 1;
}

std::optional<TimePoint> PostgresStore::lastActive(UserId user) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT (extract(epoch FROM last_active_at)*1000000)::bigint "
        "FROM user_presence WHERE user_id=$1::bigint", {number(user)});
    return result.rows() ? std::optional<TimePoint>{decodeTime(result, 0, 0)}
                         : std::nullopt;
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
            "INSERT INTO conversations(kind,title,created_by,direct_pair_key,created_at,announcement,state) "
            "VALUES($1,$2,$3::bigint,$4,to_timestamp($5::double precision/1000000.0),$6,$7) RETURNING " +
            std::string(kConversationColumns),
            {conversationKind(conversation.kind), conversation.title,
             number(conversation.createdBy), directKey, encodeTime(conversation.createdAt),
             conversation.announcement, conversationState(conversation.state)});
        auto created = readConversation(result, 0);
        for (const auto &member : members)
            (void)query(scope.connection,
                "INSERT INTO conversation_members(conversation_id,user_id,role,last_delivered_sequence,last_read_sequence,joined_at,muted_until) "
                "VALUES($1::bigint,$2::bigint,$3,$4::bigint,$5::bigint,to_timestamp($6::double precision/1000000.0),"
                "CASE WHEN $7::text IS NULL THEN NULL ELSE to_timestamp($7::double precision/1000000.0) END) RETURNING 1",
                {number(created.id), number(member.userId), memberRole(member.role),
                 number(member.lastDeliveredSequence), number(member.lastReadSequence),
                 encodeTime(member.joinedAt),
                 member.mutedUntil ? std::optional<std::string>{encodeTime(*member.mutedUntil)}
                                   : std::nullopt});
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
        " FROM conversations WHERE id=$1::bigint" +
        (activeTransactions.contains(this) ? " FOR UPDATE" : ""), {number(id)});
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
        " FROM conversations WHERE direct_pair_key=$1 AND state='active'", {key});
    return result.rows() ? std::optional<Conversation>{readConversation(result, 0)}
                         : std::nullopt;
}

std::vector<Conversation> PostgresStore::listConversations(UserId user) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kQualifiedConversationColumns) +
        " FROM conversations c JOIN conversation_members m ON m.conversation_id=c.id "
        "WHERE m.user_id=$1::bigint AND c.state='active' ORDER BY c.id", {number(user)});
    std::vector<Conversation> conversations;
    conversations.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        conversations.push_back(readConversation(result, row));
    return conversations;
}

std::vector<ConversationSummary> PostgresStore::listConversationSummaries(
    UserId user) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kQualifiedConversationColumns) +
        ",m.last_delivered_sequence,m.last_read_sequence,"
        "GREATEST(c.next_sequence-1-m.last_read_sequence,0) "
        "FROM conversations c JOIN conversation_members m ON m.conversation_id=c.id "
        "WHERE m.user_id=$1::bigint AND c.state='active' ORDER BY c.id", {number(user)});
    std::vector<ConversationSummary> summaries;
    summaries.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        summaries.push_back({readConversation(result, row),
                             std::stoull(result.value(row, 10)),
                             std::stoull(result.value(row, 11)),
                             std::stoull(result.value(row, 12))});
    return summaries;
}

std::vector<ConversationMember> PostgresStore::listMembers(
    ConversationId conversation) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kMemberColumns) + " "
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

bool PostgresStore::updateConversationProfile(
    ConversationId conversation, std::string title,
    std::string announcement)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE conversations SET title=$2,announcement=$3 "
        "WHERE id=$1::bigint AND kind='group' AND state='active' RETURNING 1",
        {number(conversation), std::move(title), std::move(announcement)}).rows() == 1;
}

bool PostgresStore::dissolveConversation(ConversationId conversation,
                                         UserId actor, TimePoint now)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE conversations SET state='dissolved',dissolved_by=$2::bigint,"
        "dissolved_at=to_timestamp($3::double precision/1000000.0) "
        "WHERE id=$1::bigint AND kind='group' AND state='active' RETURNING 1",
        {number(conversation), number(actor), encodeTime(now)}).rows() == 1;
}

bool PostgresStore::updateMemberRole(ConversationId conversation, UserId user,
                                     MemberRole role)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE conversation_members SET role=$3 "
        "WHERE conversation_id=$1::bigint AND user_id=$2::bigint RETURNING 1",
        {number(conversation), number(user), memberRole(role)}).rows() == 1;
}

bool PostgresStore::updateMemberMute(
    ConversationId conversation, UserId user,
    std::optional<TimePoint> mutedUntil)
{
    auto scope = connection();
    const std::optional<std::string> value = mutedUntil
        ? std::optional<std::string>{encodeTime(*mutedUntil)} : std::nullopt;
    return query(scope.connection,
        "UPDATE conversation_members SET muted_until="
        "CASE WHEN $3::text IS NULL THEN NULL ELSE to_timestamp($3::double precision/1000000.0) END "
        "WHERE conversation_id=$1::bigint AND user_id=$2::bigint RETURNING 1",
        {number(conversation), number(user), value}).rows() == 1;
}

bool PostgresStore::removeMember(ConversationId conversation, UserId user)
{
    auto scope = connection();
    return query(scope.connection,
        "DELETE FROM conversation_members WHERE conversation_id=$1::bigint "
        "AND user_id=$2::bigint RETURNING 1",
        {number(conversation), number(user)}).rows() == 1;
}

bool PostgresStore::updateDelivered(ConversationId conversation, UserId user,
                                    std::uint64_t sequence)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE conversation_members SET last_delivered_sequence=GREATEST(last_delivered_sequence,$3::bigint) "
        "WHERE conversation_id=$1::bigint AND user_id=$2::bigint "
        "AND last_delivered_sequence<$3::bigint RETURNING 1",
        {number(conversation), number(user), number(sequence)}).rows() == 1;
}

bool PostgresStore::updateRead(ConversationId conversation, UserId user,
                               std::uint64_t sequence)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE conversation_members SET last_read_sequence=GREATEST(last_read_sequence,$3::bigint), "
        "last_delivered_sequence=GREATEST(last_delivered_sequence,$3::bigint) "
        "WHERE conversation_id=$1::bigint AND user_id=$2::bigint "
        "AND last_read_sequence<$3::bigint RETURNING 1",
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
            return {hydrateMessage(scope.connection, readMessage(existing, 0)),
                    false};
        }

        const auto allocated = query(scope.connection,
            "UPDATE conversations SET next_sequence=next_sequence+1 WHERE id=$1::bigint "
            "AND state='active' "
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
        messages.push_back(hydrateMessage(scope.connection,
                                          readMessage(result, row)));
    return messages;
}

std::vector<Message> PostgresStore::searchMessages(
    ConversationId conversation, std::string_view text,
    std::size_t limit) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kMessageColumns) +
        " FROM messages WHERE conversation_id=$1::bigint AND state='active' "
        "AND body ILIKE '%' || $2 || '%' "
        "ORDER BY conversation_seq LIMIT $3::bigint",
        {number(conversation), std::string(text), number(limit)});
    std::vector<Message> messages;
    messages.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        messages.push_back(hydrateMessage(scope.connection,
                                          readMessage(result, row)));
    return messages;
}

std::optional<Message> PostgresStore::findMessage(MessageId id) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kMessageColumns) +
        " FROM messages WHERE id=$1::bigint", {number(id)});
    return result.rows() ? std::optional<Message>{hydrateMessage(
                               scope.connection, readMessage(result, 0))}
                         : std::nullopt;
}

RecallMessageResult PostgresStore::recallMessage(MessageId id, UserId actor,
                                                 TimePoint now)
{
    auto scope = connection();
    const auto changed = query(scope.connection,
        "UPDATE messages SET state='recalled',recalled_by=$2::bigint,"
        "recalled_at=to_timestamp($3::double precision/1000000.0) "
        "WHERE id=$1::bigint AND state='active' RETURNING " +
        std::string(kMessageColumns),
        {number(id), number(actor), encodeTime(now)});
    if (changed.rows())
        return {hydrateMessage(scope.connection, readMessage(changed, 0)), true};
    const auto existing = query(scope.connection,
        "SELECT " + std::string(kMessageColumns) +
        " FROM messages WHERE id=$1::bigint", {number(id)});
    return existing.rows() ? RecallMessageResult{hydrateMessage(
                                 scope.connection, readMessage(existing, 0)), false}
                           : RecallMessageResult{};
}

FileObject PostgresStore::createFile(FileObject file)
{
    auto scope = connection();
    const auto result = query(scope.connection,
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

std::optional<FileObject> PostgresStore::findFile(FileId id) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kFileColumns) +
        " FROM file_objects WHERE id=$1::bigint", {number(id)});
    return result.rows() ? std::optional<FileObject>{readFile(result, 0)}
                         : std::nullopt;
}

bool PostgresStore::updateFileProgress(FileId id, UserId owner,
                                       std::uint64_t expectedOffset,
                                       std::uint64_t uploadedSize)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE file_objects SET uploaded_size=$4::bigint "
        "WHERE id=$1::bigint AND owner_id=$2::bigint AND state='pending' "
        "AND uploaded_size=$3::bigint AND $4::bigint BETWEEN $3::bigint AND expected_size "
        "RETURNING 1",
        {number(id), number(owner), number(expectedOffset),
         number(uploadedSize)}).rows() == 1;
}

bool PostgresStore::completeFile(FileId id, UserId owner,
                                 std::string sha256, TimePoint now)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE file_objects SET state='ready',sha256=$3,"
        "ready_at=to_timestamp($4::double precision/1000000.0) "
        "WHERE id=$1::bigint AND owner_id=$2::bigint AND state='pending' "
        "AND uploaded_size=expected_size AND length($3)=64 RETURNING 1",
        {number(id), number(owner), std::move(sha256), encodeTime(now)}).rows() == 1;
}

bool PostgresStore::failFile(FileId id, UserId owner)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE file_objects SET state='failed' WHERE id=$1::bigint "
        "AND owner_id=$2::bigint AND state='pending' RETURNING 1",
        {number(id), number(owner)}).rows() == 1;
}

bool PostgresStore::attachFile(MessageId message, FileId file)
{
    auto scope = connection();
    return query(scope.connection,
        "INSERT INTO message_attachments(message_id,file_id,position) "
        "SELECT m.id,f.id,0 FROM messages m JOIN file_objects f ON f.id=$2::bigint "
        "WHERE m.id=$1::bigint AND f.state='ready' AND f.owner_id=m.sender_id "
        "AND NOT EXISTS(SELECT 1 FROM message_attachments a WHERE a.message_id=m.id) "
        "RETURNING 1", {number(message), number(file)}).rows() == 1;
}

std::vector<MessageAttachment> PostgresStore::attachmentsForMessage(
    MessageId message) const
{
    auto scope = connection();
    return readAttachments(scope.connection, message);
}

bool PostgresStore::canAccessFile(UserId user, FileId file) const
{
    auto scope = connection();
    return query(scope.connection,
        "SELECT 1 FROM file_objects f WHERE f.id=$2::bigint AND f.state='ready' "
        "AND (f.owner_id=$1::bigint "
        "OR EXISTS(SELECT 1 FROM users u WHERE u.avatar_file_id=f.id) "
        "OR EXISTS(SELECT 1 FROM message_attachments a "
        "JOIN messages m ON m.id=a.message_id "
        "JOIN conversation_members cm ON cm.conversation_id=m.conversation_id "
        "WHERE a.file_id=f.id AND cm.user_id=$1::bigint)) LIMIT 1",
        {number(user), number(file)}).rows() == 1;
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

bool PostgresStore::activateModelVersion(ModelNodeId node, ModelVersionId version,
                                         ModelActivationAudit audit)
{
    if (audit.nodeId != node || audit.targetVersionId != version ||
        audit.outcome != ModelActivationOutcome::Succeeded ||
        audit.actor.empty())
        return false;
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
        "), updated AS ("
        " UPDATE model_nodes SET active_version_id=(SELECT id FROM activated)"
        " WHERE id=$1::bigint AND EXISTS(SELECT 1 FROM activated) RETURNING id"
        "), audited AS ("
        " INSERT INTO model_activation_audits("
        "node_id,previous_version_id,target_version_id,action,outcome,actor,detail,created_at)"
        " SELECT $1::bigint,(SELECT active_version_id FROM locked_node),$2::bigint,"
        "$3,$4,$5,$6,to_timestamp($7::double precision/1000000.0)"
        " WHERE EXISTS(SELECT 1 FROM updated) RETURNING id"
        ") SELECT 1 FROM audited",
        {number(node), number(version), activationAction(audit.action),
         activationOutcome(audit.outcome), audit.actor, audit.detail,
         encodeTime(audit.createdAt)});
    return result.rows() == 1;
}

ModelActivationAudit PostgresStore::appendModelActivationAudit(
    ModelActivationAudit audit)
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "INSERT INTO model_activation_audits("
        "node_id,previous_version_id,target_version_id,action,outcome,actor,detail,created_at) "
        "VALUES($1::bigint,$2::bigint,$3::bigint,$4,$5,$6,$7,"
        "to_timestamp($8::double precision/1000000.0)) RETURNING " +
        std::string(kActivationAuditColumns),
        {number(audit.nodeId),
         audit.previousVersionId
             ? std::optional<std::string>{number(*audit.previousVersionId)}
             : std::nullopt,
         number(audit.targetVersionId), activationAction(audit.action),
         activationOutcome(audit.outcome), audit.actor, audit.detail,
         encodeTime(audit.createdAt)});
    return readActivationAudit(result, 0);
}

std::vector<ModelActivationAudit> PostgresStore::modelActivationAudits(
    ModelNodeId node, std::size_t limit) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kActivationAuditColumns) +
        " FROM model_activation_audits WHERE node_id=$1::bigint "
        "ORDER BY created_at DESC,id DESC LIMIT $2::bigint",
        {number(node), number(limit)});
    std::vector<ModelActivationAudit> audits;
    audits.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        audits.push_back(readActivationAudit(result, row));
    return audits;
}

bool PostgresStore::upsertGenerationAudit(AiGenerationAudit audit)
{
    auto scope = connection();
    const auto optionalTime = [](const std::optional<TimePoint> &value)
        -> std::optional<std::string>
    {
        return value ? std::optional<std::string>{encodeTime(*value)}
                     : std::nullopt;
    };
    const auto result = query(scope.connection,
        "INSERT INTO ai_generation_audits("
        "generation_id,requester_id,ai_user_id,conversation_id,model_node_id,"
        "model_version_id,runtime,model_artifact,adapter_artifact,model_checksum,"
        "prompt_sha256,prompt_bytes,output_sha256,output_bytes,output_tokens,status,"
        "error_detail,requested_at,started_at,first_token_at,completed_at,"
        "response_message_id,revision) VALUES("
        "$1,$2::bigint,$3::bigint,$4::bigint,$5::bigint,$6::bigint,$7,$8,$9,$10,"
        "$11,$12::bigint,$13,$14::bigint,$15::bigint,$16,$17,"
        "to_timestamp($18::double precision/1000000.0),"
        "to_timestamp($19::double precision/1000000.0),"
        "to_timestamp($20::double precision/1000000.0),"
        "to_timestamp($21::double precision/1000000.0),$22::bigint,$23::bigint) "
        "ON CONFLICT(generation_id) DO UPDATE SET "
        "requester_id=EXCLUDED.requester_id,ai_user_id=EXCLUDED.ai_user_id,"
        "conversation_id=EXCLUDED.conversation_id,model_node_id=EXCLUDED.model_node_id,"
        "model_version_id=EXCLUDED.model_version_id,runtime=EXCLUDED.runtime,"
        "model_artifact=EXCLUDED.model_artifact,adapter_artifact=EXCLUDED.adapter_artifact,"
        "model_checksum=EXCLUDED.model_checksum,prompt_sha256=EXCLUDED.prompt_sha256,"
        "prompt_bytes=EXCLUDED.prompt_bytes,output_sha256=EXCLUDED.output_sha256,"
        "output_bytes=EXCLUDED.output_bytes,output_tokens=EXCLUDED.output_tokens,"
        "status=EXCLUDED.status,error_detail=EXCLUDED.error_detail,"
        "requested_at=EXCLUDED.requested_at,started_at=EXCLUDED.started_at,"
        "first_token_at=EXCLUDED.first_token_at,completed_at=EXCLUDED.completed_at,"
        "response_message_id=EXCLUDED.response_message_id,revision=EXCLUDED.revision "
        "WHERE ai_generation_audits.revision<=EXCLUDED.revision RETURNING " +
        std::string(kGenerationAuditColumns),
        {audit.generationId, number(audit.requesterId), number(audit.aiUserId),
         number(audit.conversationId),
         audit.modelNodeId ? std::optional<std::string>{number(*audit.modelNodeId)}
                           : std::nullopt,
         audit.modelVersionId
             ? std::optional<std::string>{number(*audit.modelVersionId)}
             : std::nullopt,
         audit.runtime, audit.modelArtifact, audit.adapterArtifact,
         audit.modelChecksum, audit.promptSha256, number(audit.promptBytes),
         audit.outputSha256, number(audit.outputBytes), number(audit.outputTokens),
         generationStatus(audit.status), audit.errorDetail,
         encodeTime(audit.requestedAt), optionalTime(audit.startedAt),
         optionalTime(audit.firstTokenAt), optionalTime(audit.completedAt),
         audit.responseMessageId
             ? std::optional<std::string>{number(*audit.responseMessageId)}
             : std::nullopt,
         number(audit.revision)});
    return result.rows() == 1;
}

std::optional<AiGenerationAudit> PostgresStore::findGenerationAudit(
    std::string_view generationId) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kGenerationAuditColumns) +
        " FROM ai_generation_audits WHERE generation_id=$1",
        {std::string(generationId)});
    return result.rows()
               ? std::optional<AiGenerationAudit>{readGenerationAudit(result, 0)}
               : std::nullopt;
}

ModelEvaluationSuite PostgresStore::createEvaluationSuite(
    ModelEvaluationSuite suite, std::vector<ModelEvaluationCase> cases)
{
    auto scope = connection();
    const bool localTransaction = scope.lease.has_value();
    if (localTransaction) command(scope.connection, "BEGIN");
    try
    {
        const auto inserted = query(scope.connection,
            "INSERT INTO model_evaluation_suites("
            "name,version,checksum,description,created_at) "
            "VALUES($1,$2,$3,$4,to_timestamp($5::double precision/1000000.0)) RETURNING " +
            std::string(kEvaluationSuiteColumns),
            {suite.name, suite.version, suite.checksum, suite.description,
             encodeTime(suite.createdAt)});
        suite = readEvaluationSuite(inserted, 0);
        for (auto &testCase : cases)
        {
            const auto row = query(scope.connection,
                "INSERT INTO model_evaluation_cases("
                "suite_id,name,prompt,expected_answer,weight,required) "
                "VALUES($1::bigint,$2,$3,$4,$5::double precision,$6::boolean) RETURNING " +
                std::string(kEvaluationCaseColumns),
                {number(suite.id), testCase.name, testCase.prompt,
                 testCase.expectedAnswer, std::to_string(testCase.weight),
                 testCase.required ? "true" : "false"});
            testCase = readEvaluationCase(row, 0);
        }
        if (localTransaction) command(scope.connection, "COMMIT");
        return suite;
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

std::optional<ModelEvaluationSuite> PostgresStore::findEvaluationSuite(
    ModelEvaluationSuiteId id) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kEvaluationSuiteColumns) +
        " FROM model_evaluation_suites WHERE id=$1::bigint", {number(id)});
    return result.rows()
               ? std::optional<ModelEvaluationSuite>{readEvaluationSuite(result, 0)}
               : std::nullopt;
}

std::vector<ModelEvaluationCase> PostgresStore::evaluationCases(
    ModelEvaluationSuiteId suite) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kEvaluationCaseColumns) +
        " FROM model_evaluation_cases WHERE suite_id=$1::bigint ORDER BY id",
        {number(suite)});
    std::vector<ModelEvaluationCase> cases;
    cases.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        cases.push_back(readEvaluationCase(result, row));
    return cases;
}

ModelEvaluationRun PostgresStore::createEvaluationRun(ModelEvaluationRun run)
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "INSERT INTO model_evaluation_runs("
        "suite_id,model_version_id,status,total_cases,evaluated_cases,passed_cases,"
        "weighted_score,required_cases_passed,detail,started_at) VALUES("
        "$1::bigint,$2::bigint,$3,$4::bigint,$5::bigint,$6::bigint,"
        "$7::double precision,$8::boolean,$9,"
        "to_timestamp($10::double precision/1000000.0)) RETURNING " +
        std::string(kEvaluationRunColumns),
        {number(run.suiteId), number(run.modelVersionId),
         evaluationRunStatus(run.status), number(run.totalCases),
         number(run.evaluatedCases), number(run.passedCases),
         std::to_string(run.weightedScore),
         run.requiredCasesPassed ? "true" : "false", run.detail,
         encodeTime(run.startedAt)});
    return readEvaluationRun(result, 0);
}

std::optional<ModelEvaluationRun> PostgresStore::findEvaluationRun(
    ModelEvaluationRunId id) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kEvaluationRunColumns) +
        " FROM model_evaluation_runs WHERE id=$1::bigint", {number(id)});
    return result.rows()
               ? std::optional<ModelEvaluationRun>{readEvaluationRun(result, 0)}
               : std::nullopt;
}

bool PostgresStore::appendEvaluationCaseResult(
    ModelEvaluationCaseResult result)
{
    auto scope = connection();
    return query(scope.connection,
        "INSERT INTO model_evaluation_case_results("
        "run_id,case_id,score,passed,output_sha256,latency_ms,detail) "
        "SELECT r.id,c.id,$3::double precision,$4::boolean,$5,$6::bigint,$7 "
        "FROM model_evaluation_runs r "
        "JOIN model_evaluation_cases c ON c.id=$2::bigint AND c.suite_id=r.suite_id "
        "WHERE r.id=$1::bigint AND r.status='running' "
        "ON CONFLICT(run_id,case_id) DO NOTHING RETURNING " +
        std::string(kEvaluationResultColumns),
        {number(result.runId), number(result.caseId),
         std::to_string(result.score), result.passed ? "true" : "false",
         result.outputSha256, number(result.latencyMs), result.detail}).rows() == 1;
}

std::vector<ModelEvaluationCaseResult>
PostgresStore::evaluationCaseResults(ModelEvaluationRunId run) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kEvaluationResultColumns) +
        " FROM model_evaluation_case_results WHERE run_id=$1::bigint ORDER BY case_id",
        {number(run)});
    std::vector<ModelEvaluationCaseResult> results;
    results.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        results.push_back(readEvaluationCaseResult(result, row));
    return results;
}

bool PostgresStore::finishEvaluationRun(ModelEvaluationRun run)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE model_evaluation_runs SET status=$4,evaluated_cases=$5::bigint,"
        "passed_cases=$6::bigint,weighted_score=$7::double precision,"
        "required_cases_passed=$8::boolean,detail=$9,"
        "completed_at=to_timestamp($10::double precision/1000000.0) "
        "WHERE id=$1::bigint AND suite_id=$2::bigint AND model_version_id=$3::bigint "
        "AND status='running' AND $4<>'running' "
        "AND ($4<>'completed' OR (SELECT count(*) FROM model_evaluation_case_results "
        "WHERE run_id=$1::bigint)=total_cases) RETURNING 1",
        {number(run.id), number(run.suiteId), number(run.modelVersionId),
         evaluationRunStatus(run.status), number(run.evaluatedCases),
         number(run.passedCases), std::to_string(run.weightedScore),
         run.requiredCasesPassed ? "true" : "false", run.detail,
         run.completedAt ? std::optional<std::string>{encodeTime(*run.completedAt)}
                         : std::nullopt}).rows() == 1;
}

bool PostgresStore::upsertActivationPolicy(ModelActivationPolicy policy)
{
    auto scope = connection();
    return query(scope.connection,
        "INSERT INTO model_activation_policies("
        "node_id,suite_id,minimum_pass_rate,minimum_weighted_score,"
        "max_run_age_seconds,updated_at) VALUES("
        "$1::bigint,$2::bigint,$3::double precision,$4::double precision,"
        "$5::bigint,to_timestamp($6::double precision/1000000.0)) "
        "ON CONFLICT(node_id) DO UPDATE SET suite_id=EXCLUDED.suite_id,"
        "minimum_pass_rate=EXCLUDED.minimum_pass_rate,"
        "minimum_weighted_score=EXCLUDED.minimum_weighted_score,"
        "max_run_age_seconds=EXCLUDED.max_run_age_seconds,"
        "updated_at=EXCLUDED.updated_at RETURNING 1",
        {number(policy.nodeId), number(policy.suiteId),
         std::to_string(policy.minimumPassRate),
         std::to_string(policy.minimumWeightedScore),
         number(policy.maxRunAgeSeconds), encodeTime(policy.updatedAt)}).rows() == 1;
}

std::optional<ModelActivationPolicy> PostgresStore::findActivationPolicy(
    ModelNodeId node) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kActivationPolicyColumns) +
        " FROM model_activation_policies WHERE node_id=$1::bigint",
        {number(node)});
    return result.rows()
               ? std::optional<ModelActivationPolicy>{readActivationPolicy(result, 0)}
               : std::nullopt;
}

std::optional<ModelEvaluationRun>
PostgresStore::latestCompletedEvaluationRun(
    ModelVersionId version, ModelEvaluationSuiteId suite) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kEvaluationRunColumns) +
        " FROM model_evaluation_runs WHERE model_version_id=$1::bigint "
        "AND suite_id=$2::bigint AND status='completed' "
        "ORDER BY completed_at DESC,id DESC LIMIT 1",
        {number(version), number(suite)});
    return result.rows()
               ? std::optional<ModelEvaluationRun>{readEvaluationRun(result, 0)}
               : std::nullopt;
}

KnowledgeDocument PostgresStore::createKnowledgeDocument(
    KnowledgeDocument document, std::vector<KnowledgeChunk> chunks)
{
    if (chunks.empty())
        throw std::invalid_argument("knowledge document requires chunks");
    auto scope = connection();
    const bool localTransaction = scope.lease.has_value();
    if (localTransaction) command(scope.connection, "BEGIN");
    try
    {
        const auto inserted = query(scope.connection,
            "INSERT INTO knowledge_documents("
            "owner_id,scope,conversation_id,title,source_name,media_type,sha256,"
            "state,failure_detail,created_at,indexed_at) VALUES("
            "$1::bigint,$2,$3::bigint,$4,$5,$6,$7,$8,$9,"
            "to_timestamp($10::double precision/1000000.0),NULL) RETURNING " +
            std::string(kKnowledgeDocumentColumns),
            {number(document.ownerId), knowledgeScope(document.scope),
             document.conversationId
                 ? std::optional<std::string>{number(*document.conversationId)}
                 : std::nullopt,
             document.title, document.sourceName, document.mediaType,
             document.sha256, knowledgeState(document.state),
             document.failureDetail, encodeTime(document.createdAt)});
        document = readKnowledgeDocument(inserted, 0);
        for (auto &chunk : chunks)
        {
            const auto row = query(scope.connection,
                "INSERT INTO knowledge_chunks(document_id,ordinal,content,sha256) "
                "VALUES($1::bigint,$2::bigint,$3,$4) RETURNING " +
                std::string(kKnowledgeChunkColumns),
                {number(document.id), number(chunk.ordinal), chunk.content,
                 chunk.sha256});
            chunk = readKnowledgeChunk(row, 0);
        }
        if (localTransaction) command(scope.connection, "COMMIT");
        return document;
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

bool PostgresStore::updateKnowledgeDocumentState(
    KnowledgeDocumentId document, KnowledgeDocumentState expected,
    KnowledgeDocumentState next, std::string failureDetail,
    std::optional<TimePoint> indexedAt)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE knowledge_documents SET state=$3,failure_detail=$4,indexed_at="
        "CASE WHEN $5::text IS NULL THEN NULL ELSE "
        "to_timestamp($5::double precision/1000000.0) END "
        "WHERE id=$1::bigint AND state=$2 RETURNING 1",
        {number(document), knowledgeState(expected), knowledgeState(next),
         std::move(failureDetail), encodeOptionalTime(indexedAt)}).rows() == 1;
}

std::optional<KnowledgeDocument> PostgresStore::findKnowledgeDocument(
    KnowledgeDocumentId id) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kKnowledgeDocumentColumns) +
        " FROM knowledge_documents WHERE id=$1::bigint", {number(id)});
    return result.rows()
               ? std::optional<KnowledgeDocument>{readKnowledgeDocument(result, 0)}
               : std::nullopt;
}

std::optional<KnowledgeChunk> PostgresStore::findKnowledgeChunk(
    KnowledgeChunkId id) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kKnowledgeChunkColumns) +
        " FROM knowledge_chunks WHERE id=$1::bigint", {number(id)});
    return result.rows()
               ? std::optional<KnowledgeChunk>{readKnowledgeChunk(result, 0)}
               : std::nullopt;
}

std::vector<KnowledgeChunk> PostgresStore::knowledgeChunks(
    KnowledgeDocumentId document) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kKnowledgeChunkColumns) +
        " FROM knowledge_chunks WHERE document_id=$1::bigint ORDER BY ordinal",
        {number(document)});
    std::vector<KnowledgeChunk> chunks;
    chunks.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        chunks.push_back(readKnowledgeChunk(result, row));
    return chunks;
}

std::vector<KnowledgeDocument> PostgresStore::privateKnowledgeDocuments(
    UserId owner) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kKnowledgeDocumentColumns) +
        " FROM knowledge_documents WHERE owner_id=$1::bigint "
        "AND scope='private' AND state='ready' ORDER BY id", {number(owner)});
    std::vector<KnowledgeDocument> documents;
    documents.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        documents.push_back(readKnowledgeDocument(result, row));
    return documents;
}

std::vector<KnowledgeDocument>
PostgresStore::conversationKnowledgeDocuments(
    ConversationId conversation) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kKnowledgeDocumentColumns) +
        " FROM knowledge_documents WHERE conversation_id=$1::bigint "
        "AND scope='conversation' AND state='ready' ORDER BY id",
        {number(conversation)});
    std::vector<KnowledgeDocument> documents;
    documents.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        documents.push_back(readKnowledgeDocument(result, row));
    return documents;
}

std::vector<KnowledgeChunk> PostgresStore::allReadyKnowledgeChunks() const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT c.id,c.document_id,c.ordinal,c.content,c.sha256 "
        "FROM knowledge_chunks c JOIN knowledge_documents d ON d.id=c.document_id "
        "WHERE d.state='ready' ORDER BY c.document_id,c.ordinal");
    std::vector<KnowledgeChunk> chunks;
    chunks.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        chunks.push_back(readKnowledgeChunk(result, row));
    return chunks;
}

bool PostgresStore::saveRagRetrieval(
    RagRetrieval retrieval, std::vector<RagCitation> citations)
{
    auto scope = connection();
    const bool localTransaction = scope.lease.has_value();
    if (localTransaction) command(scope.connection, "BEGIN");
    try
    {
        const auto inserted = query(scope.connection,
            "INSERT INTO rag_retrievals(generation_id,requester_id,conversation_id,"
            "query_sha256,created_at) VALUES($1,$2::bigint,$3::bigint,$4,"
            "to_timestamp($5::double precision/1000000.0)) "
            "ON CONFLICT(generation_id) DO NOTHING RETURNING 1",
            {retrieval.generationId, number(retrieval.requesterId),
             number(retrieval.conversationId), retrieval.querySha256,
             encodeTime(retrieval.createdAt)});
        if (!inserted.rows())
        {
            if (localTransaction) command(scope.connection, "ROLLBACK");
            return false;
        }
        for (const auto &citation : citations)
            (void)query(scope.connection,
                "INSERT INTO rag_citations(generation_id,rank,document_id,chunk_id,score) "
                "VALUES($1,$2::bigint,$3::bigint,$4::bigint,$5::double precision) "
                "RETURNING 1",
                {retrieval.generationId, number(citation.rank),
                 number(citation.documentId), number(citation.chunkId),
                 std::to_string(citation.score)});
        if (localTransaction) command(scope.connection, "COMMIT");
        return true;
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

std::optional<RagRetrieval> PostgresStore::findRagRetrieval(
    std::string_view generationId) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kRagRetrievalColumns) +
        " FROM rag_retrievals WHERE generation_id=$1",
        {std::string(generationId)});
    return result.rows()
               ? std::optional<RagRetrieval>{readRagRetrieval(result, 0)}
               : std::nullopt;
}

std::vector<RagCitation> PostgresStore::ragCitations(
    std::string_view generationId) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kRagCitationColumns) +
        " FROM rag_citations WHERE generation_id=$1 ORDER BY rank",
        {std::string(generationId)});
    std::vector<RagCitation> citations;
    citations.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        citations.push_back(readRagCitation(result, row));
    return citations;
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

std::optional<TrainingCandidate> PostgresStore::findActiveTrainingCandidate(
    UserId owner, MessageId sourceMessage) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kCandidateColumns) +
        " FROM training_candidates WHERE owner_user_id=$1::bigint "
        "AND source_message_id=$2::bigint AND state<>'revoked' LIMIT 1",
        {number(owner), number(sourceMessage)});
    return result.rows()
               ? std::optional<TrainingCandidate>{readCandidate(result, 0)}
               : std::nullopt;
}

std::vector<TrainingCandidate> PostgresStore::trainingCandidatesForOwner(
    UserId owner) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kCandidateColumns) +
        " FROM training_candidates WHERE owner_user_id=$1::bigint ORDER BY id",
        {number(owner)});
    std::vector<TrainingCandidate> candidates;
    candidates.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        candidates.push_back(readCandidate(result, row));
    return candidates;
}

std::vector<TrainingCandidate> PostgresStore::trainingCandidatesByState(
    TrainingCandidateState state) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kCandidateColumns) +
            " FROM training_candidates WHERE state=$1 ORDER BY id",
        {trainingState(state)});
    std::vector<TrainingCandidate> candidates;
    candidates.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        candidates.push_back(readCandidate(result, row));
    return candidates;
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

bool PostgresStore::transitionTrainingCandidate(
    TrainingCandidateId id, TrainingCandidateState expected,
    TrainingCandidateState next, std::optional<DatasetVersionId> dataset)
{
    auto scope = connection();
    return query(scope.connection,
        "UPDATE training_candidates SET state=$3,dataset_version_id=$4::bigint "
        "WHERE id=$1::bigint AND state=$2 RETURNING 1",
        {number(id), trainingState(expected), trainingState(next),
         dataset ? std::optional<std::string>{number(*dataset)} : std::nullopt}
    ).rows() == 1;
}

DatasetVersion PostgresStore::createDatasetVersion(DatasetVersion version)
{
    auto scope = connection();
    const bool localTransaction = scope.lease.has_value();
    if (localTransaction) command(scope.connection, "BEGIN");
    try
    {
        const auto result = query(scope.connection,
            "INSERT INTO dataset_versions(name,checksum,created_at,artifact_path,record_count) "
            "VALUES($1,$2,to_timestamp($3::double precision/1000000.0),$4,$5::bigint) RETURNING " +
            std::string(kDatasetVersionColumns),
            {version.name, version.checksum, encodeTime(version.createdAt),
             version.artifactPath, number(version.recordCount)});
        auto persisted = readDatasetVersion(result, 0);
        persisted.candidates = version.candidates;
        for (const auto candidate : version.candidates)
            (void)query(scope.connection,
                "INSERT INTO dataset_candidates(dataset_version_id,candidate_id) VALUES($1::bigint,$2::bigint) RETURNING 1",
                {number(persisted.id), number(candidate)});
        if (localTransaction) command(scope.connection, "COMMIT");
        return persisted;
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


std::optional<DatasetVersion> PostgresStore::findDatasetVersion(
    DatasetVersionId id) const
{
    auto scope = connection();
    const auto result = query(scope.connection,
        "SELECT " + std::string(kDatasetVersionColumns) +
        " FROM dataset_versions WHERE id=$1::bigint", {number(id)});
    if (!result.rows()) return std::nullopt;
    auto version = readDatasetVersion(result, 0);
    const auto candidates = query(scope.connection,
        "SELECT candidate_id FROM dataset_candidates "
        "WHERE dataset_version_id=$1::bigint ORDER BY candidate_id",
        {number(id)});
    version.candidates.reserve(candidates.rows());
    for (int row = 0; row < candidates.rows(); ++row)
        version.candidates.push_back(std::stoull(candidates.value(row, 0)));
    return version;
}

} // namespace webserver::phase11
