#include "server/phase11/ai/ResilientModelProvider/ResilientModelProvider.h"

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace webserver::phase11
{

class ResilientModelProvider::Impl final
    : public std::enable_shared_from_this<ResilientModelProvider::Impl>
{
public:
    enum class CircuitState
    {
        Closed,
        Open,
        HalfOpen
    };

    struct Endpoint
    {
        std::shared_ptr<IModelProvider> provider;
        CircuitState circuit = CircuitState::Closed;
        std::size_t consecutiveFailures = 0;
        std::chrono::steady_clock::time_point openUntil{};
        bool halfOpenInFlight = false;
    };

    struct Attempt
    {
        std::mutex mutex;
        GenerationRequest request;
        TokenSink onToken;
        CompletionSink onComplete;
        CancellationToken cancellation;
        CancellationToken currentCancellation;
        std::chrono::steady_clock::time_point deadline{};
        std::uint64_t serial = 0;
        bool fallbackStage = false;
        bool primaryEmitted = false;
        bool stageSettled = false;
        bool completed = false;
    };

    Impl(std::shared_ptr<IModelProvider> primaryProvider,
         std::shared_ptr<IModelProvider> fallbackProvider,
         ModelProviderResilienceConfig value)
        : config(std::move(value))
    {
        if (!primaryProvider || primaryProvider->name().empty())
            throw std::invalid_argument("primary model provider is required");
        if (fallbackProvider &&
            fallbackProvider->name() != primaryProvider->name())
            throw std::invalid_argument(
                "fallback model provider must use the same runtime name");
        if (config.failureThreshold == 0 ||
            config.circuitOpenFor <= std::chrono::milliseconds::zero() ||
            config.attemptTimeout <= std::chrono::milliseconds::zero())
            throw std::invalid_argument("invalid model resilience configuration");
        runtime = primaryProvider->name();
        primary.provider = std::move(primaryProvider);
        fallback.provider = std::move(fallbackProvider);
    }

    ~Impl()
    {
        shutdown();
    }

    void startWatchdog()
    {
        watchdog = std::thread([self = shared_from_this()]
        {
            self->watchdogLoop();
        });
    }

    bool endpointReady(const Endpoint &endpoint) const noexcept
    {
        if (!endpoint.provider || !endpoint.provider->ready())
            return false;
        if (endpoint.circuit != CircuitState::Open)
            return true;
        return std::chrono::steady_clock::now() >= endpoint.openUntil;
    }

    bool ready() const noexcept
    {
        std::lock_guard lock(mutex);
        return endpointReady(primary) || endpointReady(fallback);
    }

    bool reserve(Endpoint &endpoint)
    {
        std::lock_guard lock(mutex);
        if (!endpoint.provider || !endpoint.provider->ready())
            return false;
        const auto now = std::chrono::steady_clock::now();
        if (endpoint.circuit == CircuitState::Open)
        {
            if (now < endpoint.openUntil)
            {
                ++metrics.circuitRejected;
                return false;
            }
            endpoint.circuit = CircuitState::HalfOpen;
            endpoint.halfOpenInFlight = false;
        }
        if (endpoint.circuit == CircuitState::HalfOpen)
        {
            if (endpoint.halfOpenInFlight)
            {
                ++metrics.circuitRejected;
                return false;
            }
            endpoint.halfOpenInFlight = true;
        }
        return true;
    }

    void record(Endpoint &endpoint, bool success, bool cancelled,
                bool isFallback)
    {
        std::lock_guard lock(mutex);
        if (success)
        {
            endpoint.circuit = CircuitState::Closed;
            endpoint.consecutiveFailures = 0;
            endpoint.halfOpenInFlight = false;
            if (isFallback)
                ++metrics.fallbackSuccesses;
            else
                ++metrics.primarySuccesses;
            return;
        }
        if (cancelled)
        {
            if (endpoint.circuit == CircuitState::HalfOpen)
            {
                endpoint.circuit = CircuitState::Open;
                endpoint.openUntil = std::chrono::steady_clock::now() +
                                     config.circuitOpenFor;
            }
            endpoint.halfOpenInFlight = false;
            return;
        }

        if (isFallback)
            ++metrics.fallbackFailures;
        else
            ++metrics.primaryFailures;
        ++endpoint.consecutiveFailures;
        if (endpoint.circuit == CircuitState::HalfOpen ||
            endpoint.consecutiveFailures >= config.failureThreshold)
        {
            if (endpoint.circuit != CircuitState::Open)
                ++metrics.circuitOpened;
            endpoint.circuit = CircuitState::Open;
            endpoint.openUntil = std::chrono::steady_clock::now() +
                                 config.circuitOpenFor;
        }
        endpoint.halfOpenInFlight = false;
    }

    void finish(const std::shared_ptr<Attempt> &attempt,
                GenerationCompletion completion) noexcept
    {
        CompletionSink completionSink;
        {
            std::lock_guard lock(attempt->mutex);
            if (attempt->completed)
                return;
            attempt->completed = true;
            completionSink = std::move(attempt->onComplete);
        }
        {
            std::lock_guard lock(mutex);
            if (metrics.active > 0)
                --metrics.active;
            attempts.erase(attempt->request.requestId);
        }
        changed.notify_all();
        try
        {
            completionSink(std::move(completion));
        }
        catch (...)
        {
            // 用户回调异常不能穿过 Provider 的完成线程。
        }
    }

    void start(const std::shared_ptr<Attempt> &attempt,
               bool useFallback) noexcept
    {
        Endpoint *endpoint = useFallback ? &fallback : &primary;
        if (!reserve(*endpoint))
        {
            if (!useFallback && fallback.provider)
            {
                {
                    std::lock_guard lock(mutex);
                    ++metrics.fallbackAttempts;
                }
                start(attempt, true);
                return;
            }
            finish(attempt,
                   {false, attempt->cancellation.cancelled(),
                    useFallback ? "fallback model provider is unavailable"
                                : "model provider circuit is open",
                    {}});
            return;
        }

        std::uint64_t serial = 0;
        CancellationToken endpointCancellation;
        {
            std::lock_guard lock(attempt->mutex);
            if (attempt->completed)
                return;
            if (attempt->cancellation.cancelled())
            {
                // finish() 必须在释放 attempt 锁后调用。
                serial = 0;
            }
            else
            {
                attempt->currentCancellation = CancellationToken{};
                endpointCancellation = attempt->currentCancellation;
                attempt->deadline = std::chrono::steady_clock::now() +
                                    config.attemptTimeout;
                serial = ++attempt->serial;
                attempt->stageSettled = false;
            }
            attempt->fallbackStage = useFallback;
        }

        if (serial == 0)
        {
            record(*endpoint, false, true, useFallback);
            finish(attempt, {false, true, "model generation cancelled", {}});
            return;
        }
        changed.notify_all();

        std::weak_ptr<Impl> weakSelf = shared_from_this();
        try
        {
            endpoint->provider->generate(
                attempt->request,
                [attempt, useFallback, serial](std::string_view token)
                {
                    TokenSink sink;
                    {
                        std::lock_guard lock(attempt->mutex);
                        if (attempt->completed || attempt->stageSettled ||
                            attempt->serial != serial ||
                            attempt->fallbackStage != useFallback)
                            return;
                        if (!useFallback)
                            attempt->primaryEmitted = true;
                        sink = attempt->onToken;
                    }
                    try
                    {
                        sink(token);
                    }
                    catch (...)
                    {
                        // 传输层短暂失败不破坏模型 Provider 的生命周期。
                    }
                },
                [weakSelf, attempt, endpoint, useFallback, serial](
                    GenerationCompletion completion) mutable
                {
                    const auto self = weakSelf.lock();
                    if (!self)
                        return;
                    bool primaryEmitted = false;
                    bool currentStage = false;
                    {
                        std::lock_guard lock(attempt->mutex);
                        if (attempt->completed || attempt->stageSettled ||
                            attempt->serial != serial)
                            return;
                        currentStage = attempt->fallbackStage == useFallback;
                        primaryEmitted = attempt->primaryEmitted;
                        if (currentStage)
                        {
                            // 完成、超时和外部取消只能有一个赢得这一阶段。
                            attempt->stageSettled = true;
                            attempt->deadline =
                                std::chrono::steady_clock::time_point::max();
                            ++attempt->serial;
                        }
                    }
                    if (!currentStage)
                        return;

                    const bool cancelled = completion.cancelled ||
                                           attempt->cancellation.cancelled();
                    self->record(*endpoint, completion.success, cancelled,
                                 useFallback);
                    if (!completion.success && !cancelled && !useFallback &&
                        !primaryEmitted && self->fallback.provider)
                    {
                        {
                            std::lock_guard lock(self->mutex);
                            ++self->metrics.fallbackAttempts;
                        }
                        self->start(attempt, true);
                        return;
                    }
                    if (cancelled)
                    {
                        completion.success = false;
                        completion.cancelled = true;
                        if (completion.error.empty())
                            completion.error = "model generation cancelled";
                    }
                    self->finish(attempt, std::move(completion));
                },
                endpointCancellation);
        }
        catch (const std::exception &failure)
        {
            {
                std::lock_guard lock(attempt->mutex);
                if (attempt->completed || attempt->stageSettled ||
                    attempt->serial != serial)
                    return;
                attempt->stageSettled = true;
                ++attempt->serial;
            }
            record(*endpoint, false, false, useFallback);
            if (!useFallback && fallback.provider)
            {
                {
                    std::lock_guard lock(mutex);
                    ++metrics.fallbackAttempts;
                }
                start(attempt, true);
                return;
            }
            finish(attempt, {false, false, failure.what(), {}});
        }
        catch (...)
        {
            {
                std::lock_guard lock(attempt->mutex);
                if (attempt->completed || attempt->stageSettled ||
                    attempt->serial != serial)
                    return;
                attempt->stageSettled = true;
                ++attempt->serial;
            }
            record(*endpoint, false, false, useFallback);
            if (!useFallback && fallback.provider)
            {
                {
                    std::lock_guard lock(mutex);
                    ++metrics.fallbackAttempts;
                }
                start(attempt, true);
                return;
            }
            finish(attempt, {false, false, "model provider failed", {}});
        }
    }

    GenerationHandle generate(const GenerationRequest &request,
                              TokenSink onToken,
                              CompletionSink onComplete,
                              CancellationToken cancellation)
    {
        if (!onToken || !onComplete)
            throw std::invalid_argument("model generation callbacks are required");
        auto attempt = std::make_shared<Attempt>();
        attempt->request = request;
        attempt->onToken = std::move(onToken);
        attempt->onComplete = std::move(onComplete);
        attempt->cancellation = cancellation;
        {
            std::lock_guard lock(mutex);
            if (stopping)
                throw std::runtime_error("model provider is stopping");
            if (attempts.contains(request.requestId))
                throw std::invalid_argument("duplicate model generation request id");
            ++metrics.requests;
            ++metrics.active;
            attempts.emplace(request.requestId, attempt);
        }
        start(attempt, false);
        return {request.requestId, std::move(cancellation)};
    }

    void timeout(const std::shared_ptr<Attempt> &attempt) noexcept
    {
        bool useFallback = false;
        bool emitted = false;
        CancellationToken current;
        {
            std::lock_guard lock(attempt->mutex);
            if (attempt->completed || attempt->stageSettled ||
                attempt->deadline > std::chrono::steady_clock::now())
                return;
            useFallback = attempt->fallbackStage;
            emitted = attempt->primaryEmitted;
            current = attempt->currentCancellation;
            attempt->stageSettled = true;
            ++attempt->serial; // 先让旧 Provider 的迟到回调失效。
        }
        current.cancel();
        Endpoint &endpoint = useFallback ? fallback : primary;
        record(endpoint, false, false, useFallback);
        {
            std::lock_guard lock(mutex);
            ++metrics.timedOut;
        }
        if (!useFallback && !emitted && fallback.provider)
        {
            {
                std::lock_guard lock(mutex);
                ++metrics.fallbackAttempts;
            }
            start(attempt, true);
            return;
        }
        finish(attempt, {false, false, "model provider deadline exceeded", {}});
    }

    void cancelExternally(const std::shared_ptr<Attempt> &attempt) noexcept
    {
        CancellationToken current;
        bool isFallback = false;
        {
            std::lock_guard lock(attempt->mutex);
            if (attempt->completed || attempt->stageSettled ||
                !attempt->cancellation.cancelled())
                return;
            current = attempt->currentCancellation;
            isFallback = attempt->fallbackStage;
            attempt->stageSettled = true;
            ++attempt->serial;
        }
        current.cancel();
        record(isFallback ? fallback : primary, false, true, isFallback);
        finish(attempt, {false, true, "model generation cancelled", {}});
    }

    void watchdogLoop() noexcept
    {
        std::unique_lock lock(mutex);
        while (!stopping)
        {
            changed.wait_for(lock, std::chrono::milliseconds(10));
            if (stopping)
                break;
            std::vector<std::shared_ptr<Attempt>> current;
            current.reserve(attempts.size());
            for (const auto &[_, attempt] : attempts)
                current.push_back(attempt);
            lock.unlock();
            for (const auto &attempt : current)
            {
                if (attempt->cancellation.cancelled())
                    cancelExternally(attempt);
                else
                    timeout(attempt);
            }
            lock.lock();
        }
    }

    void shutdown() noexcept
    {
        std::vector<std::shared_ptr<Attempt>> current;
        {
            std::lock_guard lock(mutex);
            if (!stopping)
            {
                stopping = true;
                current.reserve(attempts.size());
                for (const auto &[_, attempt] : attempts)
                    current.push_back(attempt);
            }
        }
        changed.notify_all();
        for (const auto &attempt : current)
        {
            CancellationToken endpointCancellation;
            {
                std::lock_guard lock(attempt->mutex);
                endpointCancellation = attempt->currentCancellation;
                attempt->stageSettled = true;
                ++attempt->serial;
            }
            endpointCancellation.cancel();
            finish(attempt,
                   {false, true, "model provider is stopping", {}});
        }
        if (watchdog.joinable() &&
            watchdog.get_id() != std::this_thread::get_id())
            watchdog.join();
    }

    ModelProviderResilienceSnapshot snapshot() const noexcept
    {
        std::lock_guard lock(mutex);
        return metrics;
    }

    std::string runtime;
    ModelProviderResilienceConfig config;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::thread watchdog;
    bool stopping = false;
    std::unordered_map<std::string, std::shared_ptr<Attempt>> attempts;
    Endpoint primary;
    Endpoint fallback;
    ModelProviderResilienceSnapshot metrics;
};

ResilientModelProvider::ResilientModelProvider(
    std::shared_ptr<IModelProvider> primary,
    std::shared_ptr<IModelProvider> fallback,
    ModelProviderResilienceConfig config)
    : impl_(std::make_shared<Impl>(std::move(primary), std::move(fallback),
                                   std::move(config)))
{
    impl_->startWatchdog();
}

ResilientModelProvider::~ResilientModelProvider()
{
    impl_->shutdown();
}

std::string ResilientModelProvider::name() const
{
    return impl_->runtime;
}

bool ResilientModelProvider::ready() const noexcept
{
    return impl_->ready();
}

GenerationHandle ResilientModelProvider::generate(
    const GenerationRequest &request, TokenSink onToken,
    CompletionSink onComplete, CancellationToken cancellation)
{
    return impl_->generate(request, std::move(onToken), std::move(onComplete),
                           std::move(cancellation));
}

ModelProviderResilienceSnapshot ResilientModelProvider::snapshot() const noexcept
{
    return impl_->snapshot();
}

} // namespace webserver::phase11
