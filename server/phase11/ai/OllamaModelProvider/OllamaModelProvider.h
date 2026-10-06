#pragma once

#include "server/phase11/ai/ModelProvider/ModelProvider.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

namespace webserver::phase11
{

struct OllamaModelProviderConfig
{
    std::string baseUrl = "http://127.0.0.1:11434";
    std::size_t maxConcurrent = 4;
    std::size_t maxQueued = 64;
    std::size_t maxRequestBytes = 1024 * 1024;
    std::size_t maxResponseLineBytes = 1024 * 1024;
    std::chrono::milliseconds connectTimeout{2000};
    std::chrono::milliseconds requestTimeout{120000};
};

/**
 * 使用一个专用 libcurl multi 线程并发调用 Ollama。
 * generate 只把任务放入有界队列，Reactor 不执行 DNS、connect 或等待模型 token。
 */
class OllamaModelProvider final : public IModelProvider
{
public:
    explicit OllamaModelProvider(OllamaModelProviderConfig config = {});
    ~OllamaModelProvider() override;

    OllamaModelProvider(const OllamaModelProvider &) = delete;
    OllamaModelProvider &operator=(const OllamaModelProvider &) = delete;

    [[nodiscard]] std::string name() const override { return "ollama"; }
    [[nodiscard]] bool ready() const noexcept override;
    GenerationHandle generate(const GenerationRequest &request,
                              TokenSink onToken,
                              CompletionSink onComplete,
                              CancellationToken cancellation = {}) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace webserver::phase11
