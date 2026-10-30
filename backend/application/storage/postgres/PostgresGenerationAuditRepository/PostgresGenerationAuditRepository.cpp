#include "backend/application/storage/postgres/PostgresGenerationAuditRepository/PostgresGenerationAuditRepository.h"
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

constexpr std::string_view kGenerationAuditColumns =
    "generation_id,requester_id,ai_user_id,conversation_id,model_node_id,"
    "model_version_id,runtime,model_artifact,adapter_artifact,model_checksum,"
    "prompt_sha256,prompt_bytes,output_sha256,output_bytes,output_tokens,status,"
    "error_detail,(extract(epoch FROM requested_at)*1000000)::bigint,"
    "(extract(epoch FROM started_at)*1000000)::bigint,"
    "(extract(epoch FROM first_token_at)*1000000)::bigint,"
    "(extract(epoch FROM completed_at)*1000000)::bigint,response_message_id,revision";
}
bool PostgresGenerationAuditRepository::upsertGenerationAudit(PGconn *connection, AiGenerationAudit audit)
{

    const auto optionalTime = [](const std::optional<TimePoint> &value)
        -> std::optional<std::string>
    {
        return value ? std::optional<std::string>{encodeTime(*value)}
                     : std::nullopt;
    };
    const auto result = query(connection,
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

std::optional<AiGenerationAudit> PostgresGenerationAuditRepository::findGenerationAudit(PGconn *connection, std::string_view generationId)
{

    const auto result = query(connection,
        "SELECT " + std::string(kGenerationAuditColumns) +
        " FROM ai_generation_audits WHERE generation_id=$1",
        {std::string(generationId)});
    return result.rows()
               ? std::optional<AiGenerationAudit>{readGenerationAudit(result, 0)}
               : std::nullopt;
}

}
