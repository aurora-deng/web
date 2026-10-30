#pragma once
#include "backend/application/ports/Ports/Ports.h"
#include "backend/application/modules/chat/domain/WorkspaceTypes/WorkspaceTypes.h"
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
namespace webserver::phase11::storage_detail {
using UserPair=std::pair<UserId,UserId>;
using MemberKey=std::pair<ConversationId,UserId>;
using IdempotencyKey=std::tuple<ConversationId,UserId,std::string>;
/** One snapshot is shared by all memory repositories. The facade owns its lock
 * and transaction snapshot, so a cross-domain rollback remains atomic. */
struct MemoryState {
        UserId nextUserId = 1;
        FriendRequestId nextFriendRequestId = 1;
        ConversationId nextConversationId = 1;
        MessageId nextMessageId = 1;
        FileId nextFileId = 1;
        OutboxEventId nextOutboxEventId = 1;
        ModelNodeId nextModelNodeId = 1;
        ModelVersionId nextModelVersionId = 1;
        ModelEdgeId nextModelEdgeId = 1;
        ModelActivationAuditId nextModelActivationAuditId = 1;
        ModelEvaluationSuiteId nextEvaluationSuiteId = 1;
        ModelEvaluationCaseId nextEvaluationCaseId = 1;
        ModelEvaluationRunId nextEvaluationRunId = 1;
        KnowledgeDocumentId nextKnowledgeDocumentId = 1;
        KnowledgeChunkId nextKnowledgeChunkId = 1;
        TrainingCandidateId nextTrainingCandidateId = 1;
        DatasetVersionId nextDatasetVersionId = 1;

        std::unordered_map<UserId, User> users;
        std::map<std::pair<UserId,std::string>,std::pair<std::string,std::string>>profileOperations;
        std::unordered_map<std::string, UserId> usernames;
        std::unordered_map<std::string, Session> sessionsById;
        std::unordered_map<std::string, std::string> sessionIdByTokenHash;
        std::unordered_map<FriendRequestId, FriendRequest> friendRequests;
        std::set<UserPair> friendships;
        std::map<UserPair, ContactSettings> contactSettings;
        std::set<UserPair> blocks;
        std::unordered_map<UserId, TimePoint> lastActive;
        std::unordered_map<ConversationId, Conversation> conversations;
        std::map<MemberKey, ConversationMember> members;
        std::map<MemberKey,webserver::chat::ConversationPreference> conversationPreferences;
        std::unordered_map<MessageId, Message> messages;
        std::unordered_map<ConversationId, std::vector<MessageId>> messageIds;
        std::map<IdempotencyKey, MessageId> idempotency;
        std::unordered_map<FileId, FileObject> files;
        std::unordered_map<MessageId, std::vector<FileId>> attachments;
        std::unordered_map<OutboxEventId, OutboxEvent> outbox;
        std::unordered_map<ModelNodeId, ModelNode> modelNodes;
        std::unordered_map<ModelVersionId, ModelVersion> modelVersions;
        std::unordered_map<ModelVersionId, ModelTrainingLineage>
            modelTrainingLineages;
        std::unordered_map<ModelEdgeId, ModelEdge> modelEdges;
        std::unordered_map<UserId, ModelBinding> modelBindings;
        std::unordered_map<ModelActivationAuditId, ModelActivationAudit>
            modelActivationAudits;
        std::unordered_map<std::string, AiGenerationAudit> generationAudits;
        std::unordered_map<ModelEvaluationSuiteId, ModelEvaluationSuite>
            evaluationSuites;
        std::unordered_map<ModelEvaluationCaseId, ModelEvaluationCase>
            evaluationCases;
        std::unordered_map<ModelEvaluationRunId, ModelEvaluationRun>
            evaluationRuns;
        std::map<std::pair<ModelEvaluationRunId, ModelEvaluationCaseId>,
                 ModelEvaluationCaseResult> evaluationResults;
        std::unordered_map<ModelNodeId, ModelActivationPolicy>
            activationPolicies;
        std::unordered_map<KnowledgeDocumentId, KnowledgeDocument>
            knowledgeDocuments;
        std::unordered_map<KnowledgeChunkId, KnowledgeChunk> knowledgeChunks;
        std::unordered_map<std::string, RagRetrieval> ragRetrievals;
        std::unordered_map<std::string, std::vector<RagCitation>> ragCitations;
        std::unordered_map<TrainingCandidateId, TrainingCandidate> trainingCandidates;
        std::unordered_map<DatasetVersionId, DatasetVersion> datasets;
    };
inline UserPair orderedUsers(UserId first,UserId second) {
    return first<second?UserPair{first,second}:UserPair{second,first};
}
}
