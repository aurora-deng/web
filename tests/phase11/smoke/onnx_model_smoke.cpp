#include "server/phase11/ai/InProcessOnnxModelProvider/InProcessOnnxModelProvider.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

using namespace webserver::phase11;

namespace
{

struct Result final
{
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<GenerationCompletion> completion;
    std::string text;
};

std::size_t parseTokenLimit(const char *raw)
{
    const std::string value(raw);
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed == 0 || parsed > 4096)
        throw std::invalid_argument("max_tokens must be between 1 and 4096");
    return static_cast<std::size_t>(parsed);
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc < 3 || argc > 5)
        {
            std::cerr << "usage: phase11_onnx_model_smoke "
                         "<model-root> <relative-model-directory> [max_tokens] "
                         "[relative-adapter.onnx_adapter]\n";
            return 2;
        }

        const std::filesystem::path modelRoot(argv[1]);
        const std::string modelArtifact(argv[2]);
        const std::size_t maxTokens = argc >= 4 ? parseTokenLimit(argv[3]) : 32;
        const std::string adapterArtifact = argc == 5 ? argv[4] : "";

        InProcessOnnxModelProviderConfig config;
        config.modelRoot = modelRoot;
        config.workerCount = 1;
        config.maxQueued = 2;
        config.maxLoadedModels = 1;
        config.maxAdaptersPerModel = 1;
        InProcessOnnxModelProvider provider(std::move(config));

        GenerationRequest request;
        request.requestId = "onnx-model-smoke";
        request.modelArtifact = modelArtifact;
        request.adapterArtifact = adapterArtifact;
        request.modelChecksum = computeOnnxArtifactChecksum(
            std::filesystem::canonical(modelRoot / modelArtifact));

        // 生产启用流程先做同样的试装。Adapter 不兼容时在这里失败，旧 Active
        // 版本仍在数据库中，不会等到用户发消息后才发现问题。
        const ModelVersion candidate{
            1, 1, "onnx-inprocess", request.modelArtifact,
            request.adapterArtifact, request.modelChecksum,
            ModelVersionState::Candidate, {}};
        const auto validation = provider.validateVersion(candidate);
        if (!validation.valid)
            throw std::runtime_error(validation.error);
        request.messages = {
            {"system", "Answer briefly and plainly."},
            {"user", "Reply with exactly: phase11 onnx ready"},
        };
        request.maxOutputTokens = maxTokens;

        Result result;
        provider.generate(
            request,
            [&result](std::string_view token)
            {
                std::lock_guard lock(result.mutex);
                result.text.append(token);
                std::cout << token << std::flush;
            },
            [&result](GenerationCompletion completion)
            {
                {
                    std::lock_guard lock(result.mutex);
                    result.completion = std::move(completion);
                }
                result.changed.notify_all();
            });

        std::unique_lock lock(result.mutex);
        if (!result.changed.wait_for(
                lock, std::chrono::minutes(10),
                [&result] { return result.completion.has_value(); }))
            throw std::runtime_error("ONNX smoke generation timed out");

        const auto completion = std::move(*result.completion);
        const auto generated = result.text;
        lock.unlock();
        std::cout << '\n';

        if (!completion.success)
        {
            std::cerr << (completion.error.empty()
                              ? "ONNX smoke generation failed"
                              : completion.error)
                      << '\n';
            return 1;
        }
        if (generated.empty())
        {
            std::cerr << "ONNX smoke completed without a decoded token\n";
            return 1;
        }

        std::cout << "ONNX smoke passed; checksum=" << request.modelChecksum
                  << ", bytes=" << generated.size() << '\n';
        return 0;
    }
    catch (const std::exception &failure)
    {
        std::cerr << "ONNX smoke failed: " << failure.what() << '\n';
        return 1;
    }
}
