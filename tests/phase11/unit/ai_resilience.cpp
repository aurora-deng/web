#include "server/phase11/ai/ResilientModelProvider/ResilientModelProvider.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

enum class ProviderMode
{
    Success,
    Failure,
    TokenThenFailure,
    Hang
};

class ScriptedProvider final : public IModelProvider
{
public:
    explicit ScriptedProvider(ProviderMode value) : mode(value) {}

    std::string name() const override { return "ollama"; }
    bool ready() const noexcept override { return true; }

    GenerationHandle generate(const GenerationRequest &request,
                              TokenSink onToken,
                              CompletionSink onComplete,
                              CancellationToken cancellation) override
    {
        ++calls;
        switch (mode.load())
        {
        case ProviderMode::Success:
            onToken("answer");
            onComplete({true, false, {}, {}});
            break;
        case ProviderMode::Failure:
            onComplete({false, false, "endpoint failed", {}});
            break;
        case ProviderMode::TokenThenFailure:
            onToken("partial");
            onComplete({false, false, "stream interrupted", {}});
            break;
        case ProviderMode::Hang:
            break;
        }
        return {request.requestId, std::move(cancellation)};
    }

    std::atomic<ProviderMode> mode;
    std::atomic_size_t calls{0};
};

class DelayedProvider final : public IModelProvider
{
public:
    ~DelayedProvider() override
    {
        for (auto &worker : workers)
            if (worker.joinable())
                worker.join();
    }

    std::string name() const override { return "ollama"; }
    bool ready() const noexcept override { return true; }

    GenerationHandle generate(const GenerationRequest &request,
                              TokenSink,
                              CompletionSink onComplete,
                              CancellationToken cancellation) override
    {
        std::lock_guard lock(mutex);
        workers.emplace_back(
            [completion = std::move(onComplete), cancellation]() mutable
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                completion({!cancellation.cancelled(), cancellation.cancelled(),
                            cancellation.cancelled() ? "cancelled" : "", {}});
            });
        return {request.requestId, std::move(cancellation)};
    }

private:
    std::mutex mutex;
    std::vector<std::thread> workers;
};

GenerationRequest request(std::string id)
{
    GenerationRequest value;
    value.requestId = std::move(id);
    value.requester = 1;
    value.messages.push_back({"user", "hello"});
    value.modelArtifact = "qwen";
    return value;
}

} // namespace

int main()
{
    auto primary = std::make_shared<ScriptedProvider>(ProviderMode::Failure);
    auto fallback = std::make_shared<ScriptedProvider>(ProviderMode::Success);
    ModelProviderResilienceConfig config;
    config.failureThreshold = 1;
    config.circuitOpenFor = std::chrono::milliseconds(25);
    // 熔断测试不应同时依赖一个极短的请求超时；Sanitizer 并行运行时，线程可能
    // 在真正调用同步 Provider 前被抢占。超时行为在下方使用独立配置测试。
    config.attemptTimeout = std::chrono::milliseconds(500);
    ResilientModelProvider provider(primary, fallback, config);

    std::vector<std::string> tokens;
    GenerationCompletion first;
    provider.generate(request("fallback-1"),
                      [&](std::string_view token) { tokens.emplace_back(token); },
                      [&](GenerationCompletion result) { first = std::move(result); },
                      {});
    CHECK(first.success);
    CHECK(tokens == std::vector<std::string>{"answer"});
    CHECK(primary->calls == 1 && fallback->calls == 1);

    // 熔断窗口内不再敲故障主端点，直接走同 Runtime 的备用端点。
    GenerationCompletion second;
    provider.generate(request("fallback-2"), [](std::string_view) {},
                      [&](GenerationCompletion result) { second = std::move(result); },
                      {});
    CHECK(second.success);
    CHECK(primary->calls == 1 && fallback->calls == 2);

    // 窗口结束后只放一个探针；成功会关闭熔断器。
    primary->mode = ProviderMode::Success;
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    GenerationCompletion recovered;
    provider.generate(request("recovered"), [](std::string_view) {},
                      [&](GenerationCompletion result)
                      { recovered = std::move(result); },
                      {});
    CHECK(recovered.success);
    CHECK(primary->calls == 2 && fallback->calls == 2);
    const auto metrics = provider.snapshot();
    CHECK(metrics.requests == 3);
    CHECK(metrics.primaryFailures == 1);
    CHECK(metrics.primarySuccesses == 1);
    CHECK(metrics.fallbackAttempts == 2);
    CHECK(metrics.fallbackSuccesses == 2);
    CHECK(metrics.circuitOpened == 1);
    CHECK(metrics.circuitRejected == 1);
    CHECK(metrics.active == 0);

    // 主模型已吐出 partial 后失败时不能换模型，否则用户会收到拼接答案。
    auto streamingPrimary =
        std::make_shared<ScriptedProvider>(ProviderMode::TokenThenFailure);
    auto unusedFallback =
        std::make_shared<ScriptedProvider>(ProviderMode::Success);
    ResilientModelProvider streaming(streamingPrimary, unusedFallback, config);
    std::vector<std::string> partial;
    GenerationCompletion interrupted;
    streaming.generate(
        request("partial"),
        [&](std::string_view token) { partial.emplace_back(token); },
        [&](GenerationCompletion result) { interrupted = std::move(result); }, {});
    CHECK(!interrupted.success && !interrupted.cancelled);
    CHECK(partial == std::vector<std::string>{"partial"});
    CHECK(unusedFallback->calls == 0);

    // 一个不回调的主端点由共享看门线程判定超时，然后才安全切到备用端点。
    auto hangingPrimary = std::make_shared<ScriptedProvider>(ProviderMode::Hang);
    auto timeoutFallback = std::make_shared<ScriptedProvider>(ProviderMode::Success);
    auto timeoutConfig = config;
    timeoutConfig.attemptTimeout = std::chrono::milliseconds(30);
    ResilientModelProvider timeoutProvider(hangingPrimary, timeoutFallback,
                                           timeoutConfig);
    std::atomic_bool timeoutCompleted{false};
    GenerationCompletion timeoutResult;
    timeoutProvider.generate(
        request("timeout-fallback"), [](std::string_view) {},
        [&](GenerationCompletion result)
        {
            timeoutResult = std::move(result);
            timeoutCompleted = true;
        }, {});
    for (int attempt = 0; attempt < 100 && !timeoutCompleted; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(timeoutCompleted && timeoutResult.success);
    CHECK(hangingPrimary->calls == 1 && timeoutFallback->calls == 1);
    CHECK(timeoutProvider.snapshot().timedOut == 1);
    CHECK(timeoutProvider.snapshot().active == 0);

    // 外部取消也由同一个看门线程清理，不要求底层 Provider 配合回调。
    auto cancelledPrimary = std::make_shared<ScriptedProvider>(ProviderMode::Hang);
    ResilientModelProvider cancelledProvider(cancelledPrimary, nullptr,
                                             timeoutConfig);
    CancellationToken cancellation;
    std::atomic_bool cancellationCompleted{false};
    GenerationCompletion cancellationResult;
    cancelledProvider.generate(
        request("external-cancel"), [](std::string_view) {},
        [&](GenerationCompletion result)
        {
            cancellationResult = std::move(result);
            cancellationCompleted = true;
        }, cancellation);
    cancellation.cancel();
    for (int attempt = 0; attempt < 100 && !cancellationCompleted; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(cancellationCompleted && cancellationResult.cancelled);
    CHECK(cancelledProvider.snapshot().active == 0);

    // 完成回调与 5 ms 看门期限故意撞在一起；每个请求仍只能得到一个终态。
    auto delayed = std::make_shared<DelayedProvider>();
    auto raceConfig = config;
    raceConfig.failureThreshold = 100;
    raceConfig.attemptTimeout = std::chrono::milliseconds(5);
    ResilientModelProvider racing(delayed, nullptr, raceConfig);
    std::atomic_size_t raceCompletions{0};
    for (std::size_t index = 0; index < 50; ++index)
        racing.generate(
            request("race-" + std::to_string(index)), [](std::string_view) {},
            [&](GenerationCompletion) { ++raceCompletions; }, {});
    for (int attempt = 0; attempt < 200 && raceCompletions != 50; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(raceCompletions == 50);
    CHECK(racing.snapshot().active == 0);
    return 0;
}
