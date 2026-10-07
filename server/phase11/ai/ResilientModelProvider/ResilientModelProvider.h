#pragma once

#include "server/phase11/ai/ModelProvider/ModelProvider.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace webserver::phase11
{

struct ModelProviderResilienceConfig
{
    std::size_t failureThreshold = 3;
    std::chrono::milliseconds circuitOpenFor{30'000};
    // Provider 自己失联时由一个共享看门线程收尾；不是“每请求一线程”。
    std::chrono::milliseconds attemptTimeout{300'000};
};

struct ModelProviderResilienceSnapshot
{
    std::uint64_t requests = 0;
    std::uint64_t primarySuccesses = 0;
    std::uint64_t primaryFailures = 0;
    std::uint64_t fallbackAttempts = 0;
    std::uint64_t fallbackSuccesses = 0;
    std::uint64_t fallbackFailures = 0;
    std::uint64_t circuitOpened = 0;
    std::uint64_t circuitRejected = 0;
    std::uint64_t timedOut = 0;
    std::size_t active = 0;
};

/**
 * 给任意 IModelProvider 套上统一的熔断与同运行时备用端点。
 *
 * 备用 Provider 必须与主 Provider 的 name() 相同，因为 ModelRouter 已经按该名称
 * 填好了模型工件。只有主端点尚未输出任何 token 时才允许切换，避免把两台模型的
 * 半段回答拼成一条看似完整、实际语义混乱的消息。
 */
class ResilientModelProvider final : public IModelProvider
{
public:
    ResilientModelProvider(
        std::shared_ptr<IModelProvider> primary,
        std::shared_ptr<IModelProvider> fallback = {},
        ModelProviderResilienceConfig config = {});
    ~ResilientModelProvider() override;

    ResilientModelProvider(const ResilientModelProvider &) = delete;
    ResilientModelProvider &operator=(const ResilientModelProvider &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] bool ready() const noexcept override;
    GenerationHandle generate(const GenerationRequest &request,
                              TokenSink onToken,
                              CompletionSink onComplete,
                              CancellationToken cancellation) override;

    [[nodiscard]] ModelProviderResilienceSnapshot snapshot() const noexcept;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace webserver::phase11
