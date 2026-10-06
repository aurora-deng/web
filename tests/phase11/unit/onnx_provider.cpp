#include "server/phase11/ai/InProcessOnnxModelProvider/InProcessOnnxModelProvider.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

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

namespace
{

class CompletionProbe final
{
public:
    CompletionSink sink()
    {
        return [this](GenerationCompletion value)
        {
            {
                std::lock_guard lock(mutex_);
                result_ = std::move(value);
                ready_ = true;
            }
            changed_.notify_all();
        };
    }

    GenerationCompletion wait()
    {
        std::unique_lock lock(mutex_);
        CHECK(changed_.wait_for(lock, std::chrono::seconds(10),
                                [this] { return ready_; }));
        return result_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    GenerationCompletion result_;
    bool ready_ = false;
};

GenerationRequest requestFor(std::string id, std::string checksum)
{
    GenerationRequest request;
    request.requestId = std::move(id);
    request.modelArtifact = "broken-model";
    request.modelChecksum = std::move(checksum);
    request.messages.push_back({"user", "hello"});
    request.maxOutputTokens = 8;
    return request;
}

} // namespace

int main()
{
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() /
                      ("phase11-onnx-provider-" + unique);
    const auto model = root / "broken-model";
    std::filesystem::create_directories(model);
    {
        std::ofstream(model / "genai_config.json") << "{}\n";
        std::ofstream(model / "weights.bin", std::ios::binary) << "abc";
    }

    const auto checksum = computeOnnxArtifactChecksum(model);
    CHECK(checksum.rfind("sha256-tree-v1:", 0) == 0);
    {
        std::ofstream(model / "weights.bin", std::ios::binary | std::ios::app)
            << "d";
    }
    CHECK(computeOnnxArtifactChecksum(model) != checksum);
    const auto currentChecksum = computeOnnxArtifactChecksum(model);
    // 与 scripts/phase11/model_checksum.py 的固定夹具结果一致，防止部署工具和
    // C++ 运行时各算出一种“同名校验和”。
    CHECK(currentChecksum ==
          "sha256-tree-v1:"
          "47671ae1349184ad76e0c9ba70e65f3150d538edd325af92bd2b48ba51b3f589");

    {
        InProcessOnnxModelProviderConfig config;
        config.modelRoot = root;
        config.workerCount = 1;
        config.maxQueued = 2;
        InProcessOnnxModelProvider provider(config);
        CHECK(provider.ready());

        // 启用前试装与真正推理共用同一套模型解析器。这个夹具故意损坏，
        // 所以校验必须失败，而且管理端错误不能泄露 modelRoot 绝对路径。
        const ModelVersion malformedVersion{
            1, 1, "onnx-inprocess", "broken-model", {}, currentChecksum,
            ModelVersionState::Candidate, {}};
        const auto validation = provider.validateVersion(malformedVersion);
        CHECK(!validation.valid);
        CHECK(validation.error.find("artifact validation failed") !=
              std::string::npos);
        CHECK(validation.error.find(root.string()) == std::string::npos);

        // 参数错误立即完成，不占推理队列。
        CompletionProbe invalidProbe;
        GenerationRequest invalid;
        invalid.requestId = "invalid";
        provider.generate(invalid, {}, invalidProbe.sink());
        const auto invalidResult = invalidProbe.wait();
        CHECK(!invalidResult.success && !invalidResult.cancelled);
        CHECK(invalidResult.error.find("model artifact") != std::string::npos);

        // 在队列执行前已经取消的任务不会触碰模型文件。
        CompletionProbe cancelledProbe;
        CancellationToken cancellation;
        cancellation.cancel();
        provider.generate(requestFor("cancelled", currentChecksum), {},
                          cancelledProbe.sink(), cancellation);
        const auto cancelled = cancelledProbe.wait();
        CHECK(!cancelled.success && cancelled.cancelled);

        // 正确摘要通过后，真实 GenAI 动态库会尝试读取配置；损坏模型必须作为
        // 异步错误返回，不能让 Worker 或服务器进程崩溃，也不能泄露模型根目录。
        CompletionProbe malformedProbe;
        provider.generate(requestFor("malformed", currentChecksum), {},
                          malformedProbe.sink());
        const auto malformed = malformedProbe.wait();
        CHECK(!malformed.success && !malformed.cancelled);
        CHECK(malformed.error.find("ONNX generation failed") != std::string::npos);
        CHECK(malformed.error.find(root.string()) == std::string::npos);
        CHECK(provider.ready());

        // 已存在但越出 modelRoot 的目录也必须被拒绝。
        const auto outside = root.parent_path() /
                             ("phase11-onnx-outside-" + unique);
        std::filesystem::create_directories(outside);
        std::ofstream(outside / "genai_config.json") << "{}\n";
        auto escapeRequest = requestFor("escape", currentChecksum);
        escapeRequest.modelArtifact = "../" + outside.filename().string();
        CompletionProbe escapeProbe;
        provider.generate(escapeRequest, {}, escapeProbe.sink());
        const auto escaped = escapeProbe.wait();
        CHECK(!escaped.success && !escaped.cancelled);
        CHECK(escaped.error.find("configured root") != std::string::npos);
        std::filesystem::remove_all(outside);
    }

    std::filesystem::remove_all(root);
    std::cout << "phase11 ONNX provider tests passed\n";
    return 0;
}
