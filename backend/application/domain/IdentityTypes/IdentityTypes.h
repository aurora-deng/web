#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace webserver::phase11 {
using UserId = std::uint64_t;
using FriendRequestId = std::uint64_t;
using ConversationId = std::uint64_t;
using MessageId = std::uint64_t;
using FileId = std::uint64_t;
using OutboxEventId = std::uint64_t;
using ModelNodeId = std::uint64_t;
using ModelVersionId = std::uint64_t;
using ModelEdgeId = std::uint64_t;
using ModelActivationAuditId = std::uint64_t;
using ModelEvaluationSuiteId = std::uint64_t;
using ModelEvaluationCaseId = std::uint64_t;
using ModelEvaluationRunId = std::uint64_t;
using KnowledgeDocumentId = std::uint64_t;
using KnowledgeChunkId = std::uint64_t;
using TrainingCandidateId = std::uint64_t;
using DatasetVersionId = std::uint64_t;
using TimePoint = std::chrono::system_clock::time_point;

}
