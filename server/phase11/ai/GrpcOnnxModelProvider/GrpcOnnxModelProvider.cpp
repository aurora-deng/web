#include "server/phase11/ai/GrpcOnnxModelProvider/GrpcOnnxModelProvider.h"

#include "server/phase11/runtime/BoundedExecutor/BoundedExecutor.h"
#include "web_learning.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace webserver::phase11
{
namespace
{

void completeSafely(const CompletionSink &sink,
                    GenerationCompletion completion) noexcept
{
    if (!sink) return;
    try
    {
        sink(std::move(completion));
    }
    catch (...)
    {
        // 上层回调异常不能越过 gRPC Worker 边界。
    }
}

std::string rpcFailure(const grpc::Status &status)
{
    // 传输错误可能带远端地址或实现细节；普通聊天只需要稳定、有限的分类。
    return "remote ONNX RPC failed (gRPC code " +
           std::to_string(static_cast<int>(status.error_code())) + ")";
}

void addAuthentication(grpc::ClientContext &context,
                       const std::string &token)
{
    if (!token.empty())
        context.AddMetadata("authorization", "Bearer " + token);
}

} // namespace

class GrpcOnnxModelProvider::Impl final
{
public:
    explicit Impl(GrpcOnnxModelProviderConfig config)
        : config_(std::move(config)),
          channel_(createChannel(config_)),
          stub_(webtest::rpc::v1::OnnxInferenceService::NewStub(channel_)),
          executor_(config_.workerCount, config_.maxQueued)
    {
        if (config_.target.empty() || config_.workerCount == 0 ||
            config_.maxQueued == 0 || config_.maxStreamBytes == 0 ||
            config_.rpcTimeout.count() <= 0 ||
            config_.validationTimeout.count() <= 0)
            throw std::invalid_argument("invalid remote ONNX provider configuration");
        cancellationThread_ = std::thread([this] { observeCancellation(); });
    }

    ~Impl()
    {
        std::vector<std::shared_ptr<Job>> jobs;
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            jobs.reserve(jobs_.size());
            for (const auto &[_, job] : jobs_)
                jobs.push_back(job);
        }
        for (const auto &job : jobs)
        {
            job->cancellation.cancel();
            job->context.TryCancel();
        }
        cancellationWake_.notify_all();
        executor_.shutdown();
        {
            std::lock_guard lock(mutex_);
            stopCancellationThread_ = true;
        }
        cancellationWake_.notify_all();
        if (cancellationThread_.joinable())
            cancellationThread_.join();
    }

    bool ready() const noexcept
    {
        std::lock_guard lock(mutex_);
        return !stopping_ && executor_.accepting() &&
               channel_->GetState(false) != GRPC_CHANNEL_SHUTDOWN;
    }

    ModelArtifactValidation validateVersion(const ModelVersion &version)
    {
        if (version.runtime != "onnx-grpc")
            return {false, "remote ONNX validator received a different runtime"};
        {
            std::lock_guard lock(mutex_);
            if (stopping_)
                return {false, "remote ONNX provider is stopping"};
        }

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() +
                             config_.validationTimeout);
        addAuthentication(context, config_.authenticationToken);
        webtest::rpc::v1::OnnxValidateRequest request;
        request.set_version_id(version.id);
        request.set_node_id(version.nodeId);
        request.set_model_artifact(version.modelArtifact);
        request.set_adapter_artifact(version.adapterArtifact);
        request.set_model_checksum(version.checksum);
        webtest::rpc::v1::OnnxValidateReply reply;
        const auto status = stub_->Validate(&context, request, &reply);
        if (!status.ok())
            return {false, rpcFailure(status)};
        std::string error = reply.error();
        if (error.size() > 512) error.resize(512);
        return {reply.valid(), std::move(error)};
    }

    GenerationHandle generate(const GenerationRequest &request,
                              TokenSink onToken,
                              CompletionSink onComplete,
                              CancellationToken cancellation)
    {
        const GenerationHandle handle{request.requestId, cancellation};
        std::string rejection;
        if (request.requestId.empty() || request.messages.empty() ||
            request.maxOutputTokens == 0 || request.maxOutputTokens > 4096)
            rejection = "invalid remote ONNX generation request";
        else if (!onComplete)
            rejection = "remote ONNX completion callback is required";
        if (!rejection.empty())
        {
            completeSafely(onComplete,
                           {false, cancellation.cancelled(),
                            std::move(rejection), {}});
            return handle;
        }

        auto job = std::make_shared<Job>();
        job->request = request;
        job->onToken = std::move(onToken);
        job->onComplete = std::move(onComplete);
        job->cancellation = cancellation;
        job->context.set_deadline(std::chrono::system_clock::now() +
                                  config_.rpcTimeout);
        addAuthentication(job->context, config_.authenticationToken);

        {
            std::lock_guard lock(mutex_);
            if (stopping_)
                rejection = "remote ONNX provider is stopping";
            else if (!jobs_.emplace(request.requestId, job).second)
                rejection = "duplicate remote ONNX request id";
        }
        if (!rejection.empty())
        {
            completeSafely(job->onComplete,
                           {false, cancellation.cancelled(),
                            std::move(rejection), {}});
            return handle;
        }

        if (!executor_.submit([this, job] { run(job); }))
        {
            {
                std::lock_guard lock(mutex_);
                jobs_.erase(request.requestId);
            }
            completeSafely(job->onComplete,
                           {false, cancellation.cancelled(),
                            "remote ONNX queue is full or stopping", {}});
        }
        cancellationWake_.notify_all();
        return handle;
    }

private:
    struct Job final
    {
        GenerationRequest request;
        TokenSink onToken;
        CompletionSink onComplete;
        CancellationToken cancellation;
        grpc::ClientContext context;
    };

    static std::shared_ptr<grpc::Channel> createChannel(
        const GrpcOnnxModelProviderConfig &config)
    {
        std::shared_ptr<grpc::ChannelCredentials> credentials;
        if (config.rootCertificates.empty())
        {
            credentials = grpc::InsecureChannelCredentials();
        }
        else
        {
            grpc::SslCredentialsOptions options;
            options.pem_root_certs = config.rootCertificates;
            credentials = grpc::SslCredentials(options);
        }
        grpc::ChannelArguments arguments;
        arguments.SetMaxReceiveMessageSize(1024 * 1024);
        arguments.SetMaxSendMessageSize(1024 * 1024);
        if (!config.tlsServerName.empty())
            arguments.SetSslTargetNameOverride(config.tlsServerName);
        return grpc::CreateCustomChannel(config.target, std::move(credentials),
                                         arguments);
    }

    void finish(const std::shared_ptr<Job> &job,
                GenerationCompletion completion) noexcept
    {
        {
            std::lock_guard lock(mutex_);
            jobs_.erase(job->request.requestId);
        }
        cancellationWake_.notify_all();
        completeSafely(job->onComplete, std::move(completion));
    }

    void run(const std::shared_ptr<Job> &job) noexcept
    {
        try
        {
            webtest::rpc::v1::OnnxGenerateRequest request;
            request.set_request_id(job->request.requestId);
            request.set_requester(job->request.requester);
            request.set_model_node(job->request.modelNode);
            request.set_max_output_tokens(job->request.maxOutputTokens);
            request.set_model_artifact(job->request.modelArtifact);
            request.set_adapter_artifact(job->request.adapterArtifact);
            request.set_model_checksum(job->request.modelChecksum);
            for (const auto &message : job->request.messages)
            {
                auto *encoded = request.add_messages();
                encoded->set_role(message.role);
                encoded->set_content(message.content);
            }

            auto reader = stub_->Generate(&job->context, request);
            webtest::rpc::v1::OnnxGenerateEvent event;
            std::size_t streamedBytes = 0;
            bool terminalSeen = false;
            GenerationCompletion terminal;
            while (reader->Read(&event))
            {
                if (job->cancellation.cancelled())
                    job->context.TryCancel();
                switch (event.payload_case())
                {
                case webtest::rpc::v1::OnnxGenerateEvent::kToken:
                    if (terminalSeen)
                        throw std::runtime_error(
                            "remote ONNX sent a token after completion");
                    if (event.token().size() >
                        config_.maxStreamBytes - streamedBytes)
                    {
                        job->context.TryCancel();
                        throw std::runtime_error(
                            "remote ONNX response exceeds configured limit");
                    }
                    streamedBytes += event.token().size();
                    if (!event.token().empty() && job->onToken)
                        job->onToken(event.token());
                    break;
                case webtest::rpc::v1::OnnxGenerateEvent::kCompleted:
                    if (terminalSeen)
                        throw std::runtime_error(
                            "remote ONNX sent duplicate completion");
                    terminalSeen = true;
                    terminal.success = event.completed().success();
                    terminal.cancelled = event.completed().cancelled();
                    terminal.error = event.completed().error();
                    if (terminal.error.size() > 512)
                        terminal.error.resize(512);
                    break;
                case webtest::rpc::v1::OnnxGenerateEvent::PAYLOAD_NOT_SET:
                    throw std::runtime_error("remote ONNX sent an empty event");
                }
            }

            const auto status = reader->Finish();
            if (job->cancellation.cancelled())
                finish(job, {false, true, "remote ONNX generation cancelled", {}});
            else if (!status.ok())
                finish(job, {false, false, rpcFailure(status), {}});
            else if (!terminalSeen)
                finish(job, {false, false,
                             "remote ONNX stream ended without completion", {}});
            else
                finish(job, std::move(terminal));
        }
        catch (const std::exception &failure)
        {
            auto error = std::string(failure.what());
            if (error.size() > 512) error.resize(512);
            finish(job, {false, job->cancellation.cancelled(),
                         job->cancellation.cancelled()
                             ? "remote ONNX generation cancelled"
                             : std::move(error),
                         {}});
        }
        catch (...)
        {
            finish(job, {false, job->cancellation.cancelled(),
                         job->cancellation.cancelled()
                             ? "remote ONNX generation cancelled"
                             : "remote ONNX generation failed",
                         {}});
        }
    }

    void observeCancellation() noexcept
    {
        std::unique_lock lock(mutex_);
        while (!stopCancellationThread_)
        {
            cancellationWake_.wait_for(lock, std::chrono::milliseconds(20));
            std::vector<std::shared_ptr<Job>> cancelled;
            for (const auto &[_, job] : jobs_)
                if (job->cancellation.cancelled())
                    cancelled.push_back(job);
            lock.unlock();
            for (const auto &job : cancelled)
                job->context.TryCancel();
            lock.lock();
        }
    }

    GrpcOnnxModelProviderConfig config_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<webtest::rpc::v1::OnnxInferenceService::Stub> stub_;
    BoundedExecutor executor_;

    mutable std::mutex mutex_;
    std::condition_variable cancellationWake_;
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs_;
    bool stopping_ = false;
    bool stopCancellationThread_ = false;
    std::thread cancellationThread_;
};

GrpcOnnxModelProvider::GrpcOnnxModelProvider(GrpcOnnxModelProviderConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

GrpcOnnxModelProvider::~GrpcOnnxModelProvider() = default;

bool GrpcOnnxModelProvider::ready() const noexcept
{
    return impl_->ready();
}

ModelArtifactValidation GrpcOnnxModelProvider::validateVersion(
    const ModelVersion &version)
{
    return impl_->validateVersion(version);
}

GenerationHandle GrpcOnnxModelProvider::generate(
    const GenerationRequest &request, TokenSink onToken,
    CompletionSink onComplete, CancellationToken cancellation)
{
    return impl_->generate(request, std::move(onToken), std::move(onComplete),
                           std::move(cancellation));
}

} // namespace webserver::phase11
