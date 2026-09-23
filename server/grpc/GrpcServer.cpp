#include "server/grpc/GrpcServer.h"

#include "server/ops/OperationalMetrics.h"
#include "server/security/AuthToken.h"
#include "web_learning.grpc.pb.h"

#include <grpcpp/alarm.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace webserver::grpc_runtime
{
namespace
{
constexpr std::size_t kMaxPayloadBytes = 64 * 1024;
constexpr std::uint32_t kMaxCountItems = 10'000;
constexpr std::uint32_t kMaxIntervalMs = 10'000;
constexpr std::uint64_t kMaxUploadBytes = 4 * 1024 * 1024;
constexpr std::uint64_t kMaxUploadChunks = 10'000;
constexpr std::uint64_t kMaxChatMessages = 10'000;

std::string readPemFile(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open gRPC TLS file: " + path);
    std::string contents{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
    if (contents.empty())
        throw std::runtime_error("gRPC TLS file is empty: " + path);
    return contents;
}

template <typename Context>
std::string metadataValue(const Context &context, std::string_view key)
{
    const auto found = context.client_metadata().find(std::string(key));
    if (found == context.client_metadata().end())
        return {};
    return {found->second.data(), found->second.length()};
}

class LearningService final
    : public ::webtest::rpc::v1::LearningService::CallbackService
{
public:
    explicit LearningService(const GrpcServerOptions &options)
        : authTokens_(options.authenticationSecret),
          maxConcurrentRpcs_(std::max<std::size_t>(1, options.maxConcurrentRpcs)),
          maxRpcDuration_(std::max(options.maxRpcDuration,
                                   std::chrono::milliseconds{1})),
          metrics_(options.metrics ? options.metrics
                                   : std::make_shared<webserver::ops::OperationalMetrics>())
    {
    }

    ::grpc::ServerUnaryReactor *Echo(
        ::grpc::CallbackServerContext *context,
        const ::webtest::rpc::v1::EchoRequest *request,
        ::webtest::rpc::v1::EchoReply *reply) override;

    ::grpc::ServerWriteReactor<::webtest::rpc::v1::CountReply> *Count(
        ::grpc::CallbackServerContext *context,
        const ::webtest::rpc::v1::CountRequest *request) override;

    ::grpc::ServerReadReactor<::webtest::rpc::v1::UploadChunk> *Upload(
        ::grpc::CallbackServerContext *context,
        ::webtest::rpc::v1::UploadSummary *summary) override;

    ::grpc::ServerBidiReactor<::webtest::rpc::v1::ChatMessage,
                              ::webtest::rpc::v1::ChatMessage> *Chat(
        ::grpc::CallbackServerContext *context) override;

    ::grpc::Status admit(::grpc::CallbackServerContext &context)
    {
        if (authTokens_.enabled())
        {
            const auto authorization = metadataValue(context, "authorization");
            const auto token = webserver::security::AuthToken::bearerToken(
                authorization);
            const auto identity = token ? authTokens_.verify(*token)
                                        : webserver::security::AuthResult{};
            if (!identity)
            {
                metrics_->grpcRejected.fetch_add(1, std::memory_order_relaxed);
                return {::grpc::StatusCode::UNAUTHENTICATED,
                        "valid bearer token required"};
            }
            context.AddInitialMetadata(
                "x-authenticated-user", std::to_string(identity.identity->userId));
            context.AddInitialMetadata(
                "x-authenticated-tenant", identity.identity->tenant);
        }

        std::size_t current = activeRpcs_.load(std::memory_order_acquire);
        while (current < maxConcurrentRpcs_ &&
               !activeRpcs_.compare_exchange_weak(
                   current, current + 1,
                   std::memory_order_acq_rel,
                   std::memory_order_acquire))
        {
        }
        if (current >= maxConcurrentRpcs_)
        {
            metrics_->grpcRejected.fetch_add(1, std::memory_order_relaxed);
            return {::grpc::StatusCode::RESOURCE_EXHAUSTED,
                    "concurrent RPC quota exhausted"};
        }
        metrics_->grpcStarted.fetch_add(1, std::memory_order_relaxed);

        auto requestId = metadataValue(context, "x-request-id");
        if (requestId.empty() || requestId.size() > 64)
        {
            requestId = "grpc-" + std::to_string(
                nextRequestId_.fetch_add(1, std::memory_order_relaxed) + 1);
        }
        context.AddInitialMetadata("x-request-id", requestId);
        return ::grpc::Status::OK;
    }

    void complete(const ::grpc::CallbackServerContext &context) noexcept
    {
        activeRpcs_.fetch_sub(1, std::memory_order_acq_rel);
        if (context.IsCancelled())
            metrics_->grpcCancelled.fetch_add(1, std::memory_order_relaxed);
        else
            metrics_->grpcCompleted.fetch_add(1, std::memory_order_relaxed);
    }

    ::grpc::Status interruption(
        const ::grpc::CallbackServerContext &context,
        std::chrono::steady_clock::time_point startedAt) const
    {
        if (context.IsCancelled())
            return {::grpc::StatusCode::CANCELLED, "request cancelled"};
        if (std::chrono::steady_clock::now() - startedAt >= maxRpcDuration_)
            return {::grpc::StatusCode::DEADLINE_EXCEEDED,
                    "server RPC duration limit exceeded"};
        return ::grpc::Status::OK;
    }

    [[nodiscard]] std::uint64_t nextSequence() noexcept
    {
        return nextSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

private:
    webserver::security::AuthToken authTokens_;
    std::size_t maxConcurrentRpcs_;
    std::chrono::milliseconds maxRpcDuration_;
    std::shared_ptr<webserver::ops::OperationalMetrics> metrics_;
    std::atomic<std::size_t> activeRpcs_{0};
    std::atomic<std::uint64_t> nextSequence_{0};
    std::atomic<std::uint64_t> nextRequestId_{0};
};

/**
 * @brief 一次 Callback RPC 的业务准入租约。
 *
 * Reactor 只在 OnDone() 中销毁，因此这个对象覆盖完整 RPC 生命周期，而不是只
 * 覆盖一次回调。这样活跃配额和完成/取消指标不会在首个回调返回时提前归还。
 */
class CallbackCall
{
public:
    CallbackCall(LearningService &service,
                 ::grpc::CallbackServerContext *context)
        : service_(service), context_(context), startedAt_(Clock::now()) {}

    ~CallbackCall()
    {
        if (admitted_)
            service_.complete(*context_);
    }

    CallbackCall(const CallbackCall &) = delete;
    CallbackCall &operator=(const CallbackCall &) = delete;

    ::grpc::Status admit()
    {
        if (attempted_)
            return admitted_ ? ::grpc::Status::OK
                             : ::grpc::Status(::grpc::StatusCode::INTERNAL,
                                              "RPC admission already failed");
        attempted_ = true;
        auto status = service_.admit(*context_);
        admitted_ = status.ok();
        return status;
    }

    [[nodiscard]] ::grpc::Status interruption() const
    {
        return service_.interruption(*context_, startedAt_);
    }

private:
    using Clock = std::chrono::steady_clock;

    LearningService &service_;
    ::grpc::CallbackServerContext *context_;
    Clock::time_point startedAt_;
    bool attempted_{false};
    bool admitted_{false};
};

class EchoReactor final : public ::grpc::ServerUnaryReactor
{
public:
    EchoReactor(LearningService &service,
                ::grpc::CallbackServerContext *context,
                const ::webtest::rpc::v1::EchoRequest *request,
                ::webtest::rpc::v1::EchoReply *reply)
        : call_(service, context)
    {
        auto status = call_.admit();
        if (status.ok() && request->message().size() > kMaxPayloadBytes)
        {
            status = {::grpc::StatusCode::INVALID_ARGUMENT,
                      "message exceeds the 64 KiB service limit"};
        }
        if (status.ok())
        {
            reply->set_message(request->message());
            reply->set_server_sequence(service.nextSequence());
        }
        Finish(std::move(status));
    }

    void OnDone() override { delete this; }
    void OnCancel() override {}

private:
    CallbackCall call_;
};

class CountReactor final
    : public ::grpc::ServerWriteReactor<::webtest::rpc::v1::CountReply>
{
public:
    CountReactor(LearningService &service,
                 ::grpc::CallbackServerContext *context,
                 const ::webtest::rpc::v1::CountRequest *request)
        : call_(service, context),
          limit_(request->limit()),
          interval_(request->interval_ms()),
          timer_(std::make_shared<TimerState>())
    {
        timer_->owner = this;
        auto status = call_.admit();
        if (status.ok() && (limit_ == 0 || limit_ > kMaxCountItems))
        {
            status = {::grpc::StatusCode::INVALID_ARGUMENT,
                      "limit must be in [1, 10000]"};
        }
        if (status.ok() && interval_.count() > kMaxIntervalMs)
        {
            status = {::grpc::StatusCode::INVALID_ARGUMENT,
                      "interval_ms must be in [0, 10000]"};
        }
        if (!status.ok())
        {
            timer_->finishing = true;
            Finish(std::move(status));
            return;
        }

        std::lock_guard lock(timer_->mutex);
        startNextWriteLocked();
    }

    void OnWriteDone(bool ok) override
    {
        ::grpc::Status finishStatus;
        bool shouldFinish = false;
        {
            std::lock_guard lock(timer_->mutex);
            if (timer_->finishing)
                return;
            if (!ok)
            {
                timer_->finishing = true;
                finishStatus = {::grpc::StatusCode::CANCELLED,
                                "client stopped reading"};
                shouldFinish = true;
            }
            else if (const auto status = call_.interruption(); !status.ok())
            {
                timer_->finishing = true;
                finishStatus = status;
                shouldFinish = true;
            }
            else if (current_ >= limit_)
            {
                timer_->finishing = true;
                finishStatus = ::grpc::Status::OK;
                shouldFinish = true;
            }
            else if (interval_ == std::chrono::milliseconds::zero())
            {
                startNextWriteLocked();
            }
            else
            {
                scheduleNextLocked();
            }
        }
        if (shouldFinish)
            Finish(std::move(finishStatus));
    }

    void OnCancel() override
    {
        finishOnce({::grpc::StatusCode::CANCELLED, "request cancelled"});
    }

    void OnDone() override
    {
        {
            std::lock_guard lock(timer_->mutex);
            timer_->owner = nullptr;
        }
        delete this;
    }

private:
    struct TimerState
    {
        std::mutex mutex;
        ::grpc::Alarm alarm;
        CountReactor *owner{nullptr};
        bool alarmPending{false};
        bool finishing{false};
        std::optional<::grpc::Status> deferredFinish;
    };

    void startNextWriteLocked()
    {
        ++current_;
        reply_.set_value(current_);
        // reply_ 必须一直活到 OnWriteDone；因此它是 Reactor 成员而不是局部变量。
        StartWrite(&reply_);
    }

    void scheduleNextLocked()
    {
        timer_->alarmPending = true;
        // Alarm 保存回调，而 TimerState 又拥有 Alarm；这里必须使用 weak_ptr，
        // 否则“state → alarm → callback → state”会形成永久引用环。
        std::weak_ptr<TimerState> weakTimer = timer_;
        timer_->alarm.Set(
            std::chrono::system_clock::now() + interval_,
            [weakTimer = std::move(weakTimer)](bool ok)
            {
                auto timer = weakTimer.lock();
                if (!timer)
                    return;
                CountReactor *owner = nullptr;
                {
                    std::lock_guard lock(timer->mutex);
                    owner = timer->owner;
                }
                if (owner)
                    owner->onAlarm(timer, ok);
            });
    }

    void onAlarm(const std::shared_ptr<TimerState> &timer, bool ok)
    {
        ::grpc::Status finishStatus;
        bool shouldFinish = false;
        {
            std::lock_guard lock(timer->mutex);
            timer->alarmPending = false;
            if (timer->finishing)
                return;
            if (timer->deferredFinish)
            {
                timer->finishing = true;
                finishStatus = std::move(*timer->deferredFinish);
                timer->deferredFinish.reset();
                shouldFinish = true;
            }
            else if (!ok)
            {
                timer->finishing = true;
                finishStatus = {::grpc::StatusCode::CANCELLED,
                                "stream timer cancelled"};
                shouldFinish = true;
            }
            else if (const auto status = call_.interruption(); !status.ok())
            {
                timer->finishing = true;
                finishStatus = status;
                shouldFinish = true;
            }
            else
            {
                startNextWriteLocked();
            }
        }
        // Finish 之后 gRPC 可以安排 OnDone，所以此调用后不再访问 Reactor 成员。
        if (shouldFinish)
            Finish(std::move(finishStatus));
    }

    void finishOnce(::grpc::Status status)
    {
        bool shouldFinish = false;
        {
            std::lock_guard lock(timer_->mutex);
            if (timer_->finishing || timer_->deferredFinish)
                return;
            if (timer_->alarmPending)
            {
                // Alarm 不属于 RPC 自身的完成计数。先取消并等回调落地，再 Finish，
                // 避免 OnDone 删除 Reactor 后定时回调仍握着悬空 this。
                timer_->deferredFinish.emplace(std::move(status));
                timer_->alarm.Cancel();
                return;
            }
            timer_->finishing = true;
            shouldFinish = true;
        }
        if (shouldFinish)
            Finish(std::move(status));
    }

    CallbackCall call_;
    std::uint32_t limit_;
    std::chrono::milliseconds interval_;
    std::shared_ptr<TimerState> timer_;
    ::webtest::rpc::v1::CountReply reply_;
    std::uint32_t current_{0};
};

class UploadReactor final
    : public ::grpc::ServerReadReactor<::webtest::rpc::v1::UploadChunk>
{
public:
    UploadReactor(LearningService &service,
                  ::grpc::CallbackServerContext *context,
                  ::webtest::rpc::v1::UploadSummary *summary)
        : call_(service, context), summary_(summary)
    {
        auto status = call_.admit();
        if (!status.ok())
        {
            finished_ = true;
            Finish(std::move(status));
            return;
        }
        StartRead(&chunk_);
    }

    void OnReadDone(bool ok) override
    {
        ::grpc::Status finishStatus;
        bool shouldFinish = false;
        {
            std::lock_guard lock(mutex_);
            if (finished_)
                return;
            if (!ok)
            {
                finished_ = true;
                finishStatus = call_.interruption();
                shouldFinish = true;
            }
            else if (const auto status = call_.interruption(); !status.ok())
            {
                finished_ = true;
                finishStatus = status;
                shouldFinish = true;
            }
            else if (chunk_.sequence() != expectedSequence_)
            {
                finished_ = true;
                finishStatus = {::grpc::StatusCode::INVALID_ARGUMENT,
                                "upload sequence must start at 1 and be contiguous"};
                shouldFinish = true;
            }
            else if (chunk_.payload().size() > kMaxPayloadBytes)
            {
                finished_ = true;
                finishStatus = {::grpc::StatusCode::INVALID_ARGUMENT,
                                "chunk exceeds the 64 KiB service limit"};
                shouldFinish = true;
            }
            else if (summary_->chunk_count() >= kMaxUploadChunks ||
                     summary_->byte_count() + chunk_.payload().size() >
                         kMaxUploadBytes)
            {
                finished_ = true;
                finishStatus = {::grpc::StatusCode::RESOURCE_EXHAUSTED,
                                "upload exceeds the bounded RPC budget"};
                shouldFinish = true;
            }
            else
            {
                summary_->set_chunk_count(summary_->chunk_count() + 1);
                summary_->set_byte_count(
                    summary_->byte_count() + chunk_.payload().size());
                summary_->set_last_sequence(chunk_.sequence());
                ++expectedSequence_;
                chunk_.Clear();
                StartRead(&chunk_);
            }
        }
        if (shouldFinish)
            Finish(std::move(finishStatus));
    }

    void OnCancel() override
    {
        finishOnce({::grpc::StatusCode::CANCELLED, "request cancelled"});
    }

    void OnDone() override { delete this; }

private:
    void finishOnce(::grpc::Status status)
    {
        {
            std::lock_guard lock(mutex_);
            if (finished_)
                return;
            finished_ = true;
        }
        Finish(std::move(status));
    }

    CallbackCall call_;
    ::webtest::rpc::v1::UploadSummary *summary_;
    std::mutex mutex_;
    ::webtest::rpc::v1::UploadChunk chunk_;
    std::uint64_t expectedSequence_{1};
    bool finished_{false};
};

class ChatReactor final
    : public ::grpc::ServerBidiReactor<::webtest::rpc::v1::ChatMessage,
                                       ::webtest::rpc::v1::ChatMessage>
{
public:
    ChatReactor(LearningService &service,
                ::grpc::CallbackServerContext *context)
        : call_(service, context)
    {
        auto status = call_.admit();
        if (!status.ok())
        {
            finished_ = true;
            Finish(std::move(status));
            return;
        }
        StartRead(&inbound_);
    }

    void OnReadDone(bool ok) override
    {
        ::grpc::Status finishStatus;
        bool shouldFinish = false;
        {
            std::lock_guard lock(mutex_);
            if (finished_)
                return;
            if (!ok)
            {
                finished_ = true;
                finishStatus = call_.interruption();
                shouldFinish = true;
            }
            else if (const auto status = call_.interruption(); !status.ok())
            {
                finished_ = true;
                finishStatus = status;
                shouldFinish = true;
            }
            else if (++messages_ > kMaxChatMessages)
            {
                finished_ = true;
                finishStatus = {::grpc::StatusCode::RESOURCE_EXHAUSTED,
                                "chat message budget exhausted"};
                shouldFinish = true;
            }
            else if (inbound_.text().size() > kMaxPayloadBytes)
            {
                finished_ = true;
                finishStatus = {::grpc::StatusCode::INVALID_ARGUMENT,
                                "chat text exceeds the 64 KiB service limit"};
                shouldFinish = true;
            }
            else
            {
                outbound_.set_sequence(inbound_.sequence());
                outbound_.set_text("echo:" + inbound_.text());
                // 同一 Reactor 同时只保留一个写操作，天然形成逐消息背压。
                StartWrite(&outbound_);
            }
        }
        if (shouldFinish)
            Finish(std::move(finishStatus));
    }

    void OnWriteDone(bool ok) override
    {
        ::grpc::Status finishStatus;
        bool shouldFinish = false;
        {
            std::lock_guard lock(mutex_);
            if (finished_)
                return;
            if (!ok)
            {
                finished_ = true;
                finishStatus = {::grpc::StatusCode::CANCELLED,
                                "client stopped reading"};
                shouldFinish = true;
            }
            else if (const auto status = call_.interruption(); !status.ok())
            {
                finished_ = true;
                finishStatus = status;
                shouldFinish = true;
            }
            else
            {
                inbound_.Clear();
                StartRead(&inbound_);
            }
        }
        if (shouldFinish)
            Finish(std::move(finishStatus));
    }

    void OnCancel() override
    {
        finishOnce({::grpc::StatusCode::CANCELLED, "request cancelled"});
    }

    void OnDone() override { delete this; }

private:
    void finishOnce(::grpc::Status status)
    {
        {
            std::lock_guard lock(mutex_);
            if (finished_)
                return;
            finished_ = true;
        }
        Finish(std::move(status));
    }

    CallbackCall call_;
    std::mutex mutex_;
    ::webtest::rpc::v1::ChatMessage inbound_;
    ::webtest::rpc::v1::ChatMessage outbound_;
    std::uint64_t messages_{0};
    bool finished_{false};
};

::grpc::ServerUnaryReactor *LearningService::Echo(
    ::grpc::CallbackServerContext *context,
    const ::webtest::rpc::v1::EchoRequest *request,
    ::webtest::rpc::v1::EchoReply *reply)
{
    return new EchoReactor(*this, context, request, reply);
}

::grpc::ServerWriteReactor<::webtest::rpc::v1::CountReply> *
LearningService::Count(
    ::grpc::CallbackServerContext *context,
    const ::webtest::rpc::v1::CountRequest *request)
{
    return new CountReactor(*this, context, request);
}

::grpc::ServerReadReactor<::webtest::rpc::v1::UploadChunk> *
LearningService::Upload(
    ::grpc::CallbackServerContext *context,
    ::webtest::rpc::v1::UploadSummary *summary)
{
    return new UploadReactor(*this, context, summary);
}

::grpc::ServerBidiReactor<::webtest::rpc::v1::ChatMessage,
                          ::webtest::rpc::v1::ChatMessage> *
LearningService::Chat(::grpc::CallbackServerContext *context)
{
    return new ChatReactor(*this, context);
}

std::shared_ptr<::grpc::ServerCredentials> makeCredentials(
    const GrpcServerOptions &options)
{
    const bool hasCertificate = !options.certificateChainPath.empty();
    const bool hasPrivateKey = !options.privateKeyPath.empty();
    if (hasCertificate != hasPrivateKey)
        throw std::invalid_argument(
            "gRPC TLS requires both certificateChainPath and privateKeyPath");
    if (!hasCertificate)
        return ::grpc::InsecureServerCredentials();

    ::grpc::SslServerCredentialsOptions tls;
    tls.pem_key_cert_pairs.push_back(
        {readPemFile(options.privateKeyPath),
         readPemFile(options.certificateChainPath)});
    return ::grpc::SslServerCredentials(tls);
}
} // namespace

struct GrpcServer::Impl
{
    explicit Impl(GrpcServerOptions value)
        : options(std::move(value)), service(options) {}

    GrpcServerOptions options;
    LearningService service;
    std::unique_ptr<::grpc::Server> server;
    int boundPort{0};
};

GrpcServer::GrpcServer(GrpcServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
    if (impl_->options.address.empty())
        throw std::invalid_argument("gRPC address must not be empty");
    if (impl_->options.maxReceiveMessageBytes <= 0 ||
        impl_->options.maxSendMessageBytes <= 0)
        throw std::invalid_argument("gRPC message limits must be positive");
    if (impl_->options.maxConcurrentRpcs == 0)
        throw std::invalid_argument("gRPC concurrent RPC limit must be positive");
    if (impl_->options.maxWorkerThreads == 0 ||
        impl_->options.maxWorkerThreads >
            static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("gRPC worker thread limit is invalid");
    if (impl_->options.maxRpcDuration <= std::chrono::milliseconds::zero())
        throw std::invalid_argument("gRPC duration limit must be positive");
}

GrpcServer::~GrpcServer()
{
    stop();
}

void GrpcServer::start()
{
    if (impl_->server)
        throw std::logic_error("gRPC server is already running");

    ::grpc::EnableDefaultHealthCheckService(true);
    ::grpc::ServerBuilder builder;
    // Callback API 的 Reactor 在 Read/Write/Alarm 完成后才被重新调度，不会让每条
    // 长流永久占住一条业务线程。ResourceQuota 继续限制 gRPC Core 的运行时线程，
    // CallbackCall 则独立限制活跃 RPC 数量，两层保护不同资源。
    ::grpc::ResourceQuota resourceQuota;
    resourceQuota.SetMaxThreads(static_cast<int>(impl_->options.maxWorkerThreads));
    builder.SetResourceQuota(resourceQuota);
    builder.SetMaxReceiveMessageSize(impl_->options.maxReceiveMessageBytes);
    builder.SetMaxSendMessageSize(impl_->options.maxSendMessageBytes);
    builder.RegisterService(&impl_->service);
    builder.AddListeningPort(
        impl_->options.address, makeCredentials(impl_->options), &impl_->boundPort);

    impl_->server = builder.BuildAndStart();
    if (!impl_->server || impl_->boundPort == 0)
    {
        impl_->server.reset();
        impl_->boundPort = 0;
        throw std::runtime_error("gRPC failed to bind " + impl_->options.address);
    }
}

void GrpcServer::stop(std::chrono::milliseconds grace) noexcept
{
    if (!impl_ || !impl_->server)
        return;
    impl_->server->Shutdown(std::chrono::system_clock::now() + grace);
    impl_->server->Wait();
    impl_->server.reset();
    impl_->boundPort = 0;
}

bool GrpcServer::running() const noexcept
{
    return impl_ && static_cast<bool>(impl_->server);
}

int GrpcServer::boundPort() const noexcept
{
    return impl_ ? impl_->boundPort : 0;
}

const std::string &GrpcServer::configuredAddress() const noexcept
{
    return impl_->options.address;
}

} // namespace webserver::grpc_runtime
