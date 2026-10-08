#include "server/phase11/ai/DeterministicExactScorer/DeterministicExactScorer.h"
#include "server/phase11/ai/InProcessOnnxModelProvider/InProcessOnnxModelProvider.h"
#include "server/phase11/ai/ModelEvaluationService/ModelEvaluationService.h"
#include "server/phase11/ai/ModelRegistryService/ModelRegistryService.h"
#include "server/phase11/storage/postgres/PostgresStore/PostgresStore.h"

#include <openssl/evp.h>

#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace webserver::phase11;

namespace
{

std::uint64_t positive(std::string_view value, std::string_view name,
                       std::uint64_t maximum =
                           std::numeric_limits<std::uint64_t>::max())
{
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                        parsed);
    if (value.empty() || result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() || parsed == 0 ||
        parsed > maximum)
        throw std::invalid_argument(std::string(name) +
                                    " must be a positive integer");
    return parsed;
}

std::string sha256(std::string_view value)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_Digest(value.data(), value.size(), digest.data(), &size,
                   EVP_sha256(), nullptr) != 1 || size != 32)
        throw std::runtime_error("cannot calculate output SHA-256");
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < size; ++index)
        encoded << std::setw(2) << static_cast<unsigned int>(digest[index]);
    return encoded.str();
}

void requireRelative(const std::filesystem::path &path,
                     std::string_view description)
{
    if (path.empty() || path.is_absolute() || path.has_root_path())
        throw std::invalid_argument(std::string(description) +
                                    " must be a relative path");
    for (const auto &part : path)
        if (part == "..")
            throw std::invalid_argument(std::string(description) +
                                        " cannot escape the model root");
}

InProcessOnnxModelProviderConfig providerConfig(
    const std::filesystem::path &modelRoot)
{
    InProcessOnnxModelProviderConfig config;
    config.modelRoot = std::filesystem::canonical(modelRoot);
    config.workerCount = 1;
    config.maxQueued = 2;
    config.maxLoadedModels = 1;
    config.maxAdaptersPerModel = 2;
    return config;
}

struct Generated final
{
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<GenerationCompletion> completion;
    std::string text;
};

std::pair<std::string, std::uint64_t> generate(
    IModelProvider &provider, const ModelVersion &version,
    const ModelEvaluationCase &testCase, std::size_t maxTokens,
    std::chrono::seconds timeout)
{
    GenerationRequest request;
    request.requestId = "evaluation-" + std::to_string(testCase.id);
    request.modelNode = version.nodeId;
    request.modelArtifact = version.modelArtifact;
    request.adapterArtifact = version.adapterArtifact;
    request.modelChecksum = version.checksum;
    // 评测上下文必须固定，否则同一道题因调用侧 System Prompt 不同而不可复现。
    // 该值与项目的 ONNX smoke 保持一致，Suite 只负责定义用户题目。
    request.messages = {
        {"system", "Answer briefly and plainly."},
        {"user", testCase.prompt},
    };
    request.maxOutputTokens = maxTokens;

    auto result = std::make_shared<Generated>();
    CancellationToken cancellation;
    const auto started = std::chrono::steady_clock::now();
    provider.generate(
        request,
        [result](std::string_view token)
        {
            std::lock_guard lock(result->mutex);
            result->text.append(token);
        },
        [result](GenerationCompletion completion)
        {
            {
                std::lock_guard lock(result->mutex);
                result->completion = std::move(completion);
            }
            result->changed.notify_all();
        }, cancellation);

    std::unique_lock lock(result->mutex);
    if (!result->changed.wait_for(
            lock, timeout, [&result] { return result->completion.has_value(); }))
    {
        cancellation.cancel();
        if (!result->changed.wait_for(
                lock, std::chrono::seconds(30),
                [&result] { return result->completion.has_value(); }))
            throw std::runtime_error(
                "evaluation generation ignored cancellation after timeout");
        throw std::runtime_error("evaluation generation timed out");
    }
    auto completion = std::move(*result->completion);
    auto text = std::move(result->text);
    lock.unlock();
    if (!completion.success)
        throw std::runtime_error(completion.error.empty()
                                     ? "evaluation generation failed"
                                     : completion.error);
    const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    return {std::move(text), static_cast<std::uint64_t>(latency)};
}

void usage()
{
    std::cerr
        << "usage:\n"
        << "  phase11-model-admin register-onnx <node-id> <model-root> "
           "<relative-model-dir> <relative-adapter|->\n"
        << "  phase11-model-admin evaluate-onnx <version-id> <model-root> "
           "[max-tokens] [case-timeout-seconds]\n"
        << "  phase11-model-admin activate-onnx <node-id> <version-id> "
           "<model-root> <actor>\n";
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        const char *database = std::getenv("WEB_PHASE11_DATABASE");
        if (!database || !*database)
            throw std::invalid_argument("WEB_PHASE11_DATABASE is required");
        if (argc < 2)
        {
            usage();
            return 2;
        }
        PostgresStore store({database, 2, std::chrono::seconds(5)});
        if (!store.healthy())
            throw std::runtime_error("PostgreSQL is not healthy");
        const std::string_view command(argv[1]);

        if (command == "register-onnx" && argc == 6)
        {
            const auto node = positive(argv[2], "node ID");
            const std::filesystem::path modelRoot(argv[3]);
            const std::filesystem::path modelArtifact(argv[4]);
            requireRelative(modelArtifact, "model directory");
            std::string adapterArtifact;
            if (std::string_view(argv[5]) != "-")
            {
                const std::filesystem::path adapter(argv[5]);
                requireRelative(adapter, "adapter file");
                if (adapter.extension() != ".onnx_adapter")
                    throw std::invalid_argument(
                        "adapter file must end in .onnx_adapter");
                adapterArtifact = adapter.generic_string();
            }
            const auto root = std::filesystem::canonical(modelRoot);
            const auto modelDirectory = std::filesystem::canonical(
                root / modelArtifact);
            const auto relativeCheck = std::filesystem::relative(
                modelDirectory, root);
            requireRelative(relativeCheck, "resolved model directory");
            if (!adapterArtifact.empty())
            {
                const auto adapterPath = std::filesystem::canonical(
                    modelDirectory / adapterArtifact);
                const auto relativeAdapter = std::filesystem::relative(
                    adapterPath, modelDirectory);
                requireRelative(relativeAdapter, "resolved adapter file");
                if (!std::filesystem::is_regular_file(adapterPath))
                    throw std::invalid_argument(
                        "adapter artifact does not exist");
            }

            ModelRegistryService registry(store);
            const auto version = registry.registerVersion(
                node, "onnx-inprocess", modelArtifact.generic_string(),
                adapterArtifact, computeOnnxArtifactChecksum(modelDirectory),
                std::chrono::system_clock::now());
            std::cout << "version_id=" << version.id
                      << " state=candidate checksum=" << version.checksum
                      << '\n';
            return 0;
        }

        if (command == "evaluate-onnx" && argc >= 4 && argc <= 6)
        {
            const auto versionId = positive(argv[2], "version ID");
            const std::size_t maxTokens = argc >= 5
                ? positive(argv[4], "max tokens", 4096) : 256;
            const auto timeout = std::chrono::seconds(argc == 6
                ? positive(argv[5], "case timeout", 3600) : 600);
            const auto version = store.findModelVersion(versionId);
            if (!version || version->state != ModelVersionState::Candidate)
                throw std::invalid_argument(
                    "evaluation target must be a candidate model version");
            if (version->runtime != "onnx-inprocess")
                throw std::invalid_argument(
                    "evaluate-onnx only supports onnx-inprocess versions");
            const auto policy = store.findActivationPolicy(version->nodeId);
            if (!policy)
                throw std::invalid_argument(
                    "model node has no activation evaluation policy");
            const auto cases = store.evaluationCases(policy->suiteId);
            if (cases.empty())
                throw std::invalid_argument("activation suite has no cases");
            for (const auto &testCase : cases)
                if (testCase.expectedAnswer.empty())
                    throw std::invalid_argument(
                        "normalized-exact-v1 requires every expected answer");

            InProcessOnnxModelProvider provider(providerConfig(argv[3]));
            const auto validation = provider.validateVersion(*version);
            if (!validation.valid)
                throw std::runtime_error(validation.error);
            ModelEvaluationService evaluations(store, store);
            const auto run = evaluations.startRun(
                version->id, policy->suiteId,
                std::chrono::system_clock::now());
            DeterministicExactScorer scorer;
            try
            {
                for (const auto &testCase : cases)
                {
                    auto [output, latency] = generate(
                        provider, *version, testCase, maxTokens, timeout);
                    const auto result = scorer.score(
                        testCase.expectedAnswer, output);
                    evaluations.recordResult(
                        run.id, testCase.id, result.score, result.passed,
                        sha256(output), latency,
                        std::string("scorer=") +
                            std::string(DeterministicExactScorer::id()) +
                            "; " + result.detail);
                    std::cout << "run_id=" << run.id
                              << " case_id=" << testCase.id
                              << " score=" << result.score
                              << " passed=" << (result.passed ? "true" : "false")
                              << " latency_ms=" << latency << '\n';
                }
                const auto completed = evaluations.completeRun(
                    run.id, std::chrono::system_clock::now());
                std::cout << "run_id=" << completed.id
                          << " status=completed passed="
                          << completed.passedCases << '/' << completed.totalCases
                          << " weighted_score=" << completed.weightedScore
                          << " scorer=" << DeterministicExactScorer::id()
                          << '\n';
            }
            catch (const std::exception &failure)
            {
                try
                {
                    evaluations.abortRun(
                        run.id, std::string("evaluation worker failed: ") +
                                    failure.what(),
                        std::chrono::system_clock::now());
                }
                catch (...) {}
                throw;
            }
            return 0;
        }

        if (command == "activate-onnx" && argc == 6)
        {
            const auto node = positive(argv[2], "node ID");
            const auto version = positive(argv[3], "version ID");
            auto provider = std::make_shared<InProcessOnnxModelProvider>(
                providerConfig(argv[4]));
            ModelEvaluationService qualityGate(store, store);
            ModelRegistryService registry(store, &qualityGate);
            registry.registerValidator(provider);
            registry.activate(node, version, argv[5]);
            std::cout << "node_id=" << node << " version_id=" << version
                      << " state=active actor=" << argv[5] << '\n';
            return 0;
        }

        usage();
        return 2;
    }
    catch (const std::exception &failure)
    {
        std::cerr << "phase11-model-admin: " << failure.what() << '\n';
        return 1;
    }
}
