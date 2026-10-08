#include "server/phase11/business/ChatService/ChatService.h"
#include "server/phase11/storage/InMemoryStore/InMemoryStore.h"
#include "server/phase11/training/JsonlTrainingDataExporter/JsonlTrainingDataExporter.h"
#include "server/phase11/training/TrainingDataService/TrainingDataService.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>

using namespace webserver::phase11;

#define CHECK(expression)                                                       \
    do                                                                          \
    {                                                                           \
        if (!(expression))                                                      \
        {                                                                       \
            std::cerr << "CHECK failed: " #expression << " at " << __FILE__    \
                      << ':' << __LINE__ << '\n';                               \
            std::abort();                                                       \
        }                                                                       \
    } while (false)

int main()
{
    const auto now = std::chrono::system_clock::now();
    InMemoryStore store;
    ChatService chat(store, store, store, store, store, store, store);
    TrainingDataService training(store, store, store, store, store);

    User owner;
    owner.username = "training-owner";
    owner.displayName = "Training Owner";
    owner.passwordHash = "disabled";
    owner.createdAt = now;
    owner = store.create(std::move(owner));
    User ai;
    ai.username = "training-model";
    ai.displayName = "Training Model";
    ai.passwordHash = "disabled";
    ai.aiAccount = true;
    ai.createdAt = now;
    ai = store.create(std::move(ai));

    const auto conversation = chat.createDirectConversation(owner.id, ai.id, now);
    const auto response = chat.sendText(
        ai.id, conversation.id, "training-source", "raw response", now);
    auto candidate = training.submitTrainingCandidate(
        {owner.id, response.message.id, "How does \"commit\" work?",
         "Write both rows together.\nThen commit.", "consent-v1", true}, now);
    bool duplicateRejected = false;
    try
    {
        (void)training.submitTrainingCandidate(
            {owner.id, response.message.id, "duplicate", "duplicate",
             "consent-v1", true}, now);
    }
    catch (const ApplicationError &failure)
    {
        duplicateRejected = failure.code() == ErrorCode::Conflict;
    }
    CHECK(duplicateRejected);
    candidate = training.reviewTrainingCandidate(candidate.id, true);
    CHECK(store.trainingCandidatesByState(
              TrainingCandidateState::Approved).size() == 1);

    const auto root = std::filesystem::temp_directory_path() /
        ("phase22-training-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    JsonlTrainingDataExporter exporter({root, 1024U * 1024U});
    const auto dataset = training.createDataset(
        "phase22-training-v1", {candidate.id}, now, exporter);
    CHECK(dataset.recordCount == 1);
    CHECK(dataset.checksum.size() == 64);
    CHECK(dataset.artifactPath.starts_with("dataset-") &&
          dataset.artifactPath.ends_with(".jsonl"));

    const auto path = root / dataset.artifactPath;
    CHECK(std::filesystem::is_regular_file(path));
    std::ifstream input(path, std::ios::binary);
    const std::string content(std::istreambuf_iterator<char>(input), {});
    CHECK(content.find("phase22-chat-jsonl-v1") != std::string::npos);
    CHECK(content.find("How does \\\"commit\\\" work?") != std::string::npos);
    CHECK(content.find("together.\\nThen commit.") != std::string::npos);

    // 内容寻址导出是幂等的：相同样本只复用同一文件。
    const auto repeated = exporter.exportDataset("ignored-display-name",
                                                  {candidate});
    CHECK(repeated.relativePath == dataset.artifactPath);
    CHECK(repeated.sha256 == dataset.checksum);

    // 多个数据库 Worker 可能同时导出相同样本。所有调用必须收敛到同一份完整文件。
    std::vector<std::future<TrainingDatasetArtifact>> concurrent;
    for (int index = 0; index < 8; ++index)
        concurrent.push_back(std::async(std::launch::async, [&]
        {
            return exporter.exportDataset("concurrent", {candidate});
        }));
    for (auto &result : concurrent)
    {
        const auto artifact = result.get();
        CHECK(artifact.relativePath == dataset.artifactPath);
        CHECK(artifact.sha256 == dataset.checksum);
    }
    const auto persisted = store.findDatasetVersion(dataset.id);
    CHECK(persisted && persisted->artifactPath == dataset.artifactPath &&
          persisted->candidates == std::vector<TrainingCandidateId>{candidate.id});

    const auto revoked = training.revokeTrainingCandidate(candidate.id, owner.id);
    CHECK(revoked.state == TrainingCandidateState::Revoked);
    const auto resubmitted = training.submitTrainingCandidate(
        {owner.id, response.message.id, "new sanitized prompt",
         "new sanitized response", "consent-v1", true}, now);
    CHECK(resubmitted.id != candidate.id);
    CHECK(training.trainingCandidates(owner.id).size() == 2);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::cout << "phase22 training dataset tests passed\n";
    return 0;
}
