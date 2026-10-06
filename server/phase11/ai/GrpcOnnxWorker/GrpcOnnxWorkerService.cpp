#include "server/phase11/ai/GrpcOnnxWorker/GrpcOnnxWorkerService.h"

#include <openssl/crypto.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace webserver::phase11
{
namespace
{

constexpr std::size_t kMaxPromptBytes = 1024 * 1024;
constexpr std::size_t kMaxPromptMessages = 64;

std::optional<std::string_view> metadataValue(grpc::ServerContext &context,
                                              std::string_view key)
{
    const auto &metadata = context.client_metadata();
    const auto found = metadata.find(grpc::string_ref(key.data(), key.size()));
    if (found == metadata.end()) return std::nullopt;
    return std::string_view(found->second.data(), found->second.size());
}

bool validRole(std::string_view role)
{
    return role == "system" || role == "user" || role == "assistant" ||
           role == "tool";
}

struct StreamState final
{
    explicit StreamState(std::size_t tokenLimit, std::size_t byteLimit)
        : maxTokens(tokenLimit), maxBytes(byteLimit)
    {
    }

    void push(std::string_view token)
    {
        if (token.empty()) return;
        std::unique_lock lock(mutex);
        if (token.size() > maxBytes)
        {
            overflowed = true;
            cancellation.cancel();
            changed.notify_all();
            return;
        }
        changed.wait(lock, [&]
        {
            return closed || overflowed || cancellation.cancelled() ||
                   (tokens.size() < maxTokens &&
                    bufferedBytes <= maxBytes - token.size());
        });
        if (closed || overflowed || cancellation.cancelled()) return;
        tokens.emplace_back(token);
        bufferedBytes += token.size();
        changed.notify_all();
    }

    void complete(GenerationCompletion value)
    {
        std::lock_guard lock(mutex);
        if (!completion) completion = std::move(value);
        changed.notify_all();
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::string> tokens;
    std::size_t bufferedBytes = 0;
    std::optional<GenerationCompletion> completion;
    CancellationToken cancellation;
    const std::size_t maxTokens;
    const std::size_t maxBytes;
    bool overflowed = false;
    bool closed = false;
};

} // namespace

GrpcOnnxWorkerService::GrpcOnnxWorkerService(
    std::shared_ptr<IModelProvider> provider,
    std::shared_ptr<IModelArtifactValidator> validator,
    GrpcOnnxWorkerServiceConfig config)
    : provider_(std::move(provider)), validator_(std::move(validator)),
      config_(std::move(config))
{
    if (!provider_ || !validator_ || config_.maxBufferedTokens == 0 ||
        config_.maxBufferedBytes == 0)
        throw std::invalid_argument("invalid ONNX worker service configuration");
}

bool GrpcOnnxWorkerService::authenticated(
    grpc::ServerContext &context) const noexcept
{
    if (config_.authenticationToken.empty()) return true;
    const auto authorization = metadataValue(context, "authorization");
    constexpr std::string_view prefix = "Bearer ";
    if (!authorization || authorization->size() !=
                              prefix.size() + config_.authenticationToken.size() ||
        authorization->substr(0, prefix.size()) != prefix)
        return false;
    const auto candidate = authorization->substr(prefix.size());
    return CRYPTO_memcmp(candidate.data(), config_.authenticationToken.data(),
                         candidate.size()) == 0;
}

grpc::Status GrpcOnnxWorkerService::Generate(
    grpc::ServerContext *context,
    const webtest::rpc::v1::OnnxGenerateRequest *request,
    grpc::ServerWriter<webtest::rpc::v1::OnnxGenerateEvent> *writer)
{
    if (!context || !request || !writer)
        return {grpc::StatusCode::INTERNAL, "invalid RPC state"};
    if (!authenticated(*context))
        return {grpc::StatusCode::UNAUTHENTICATED, "invalid inference token"};
    if (request->request_id().empty() || request->request_id().size() > 125 ||
        request->messages().empty() ||
        request->messages_size() > static_cast<int>(kMaxPromptMessages) ||
        request->max_output_tokens() == 0 || request->max_output_tokens() > 4096 ||
        request->model_artifact().empty() || request->model_checksum().empty())
        return {grpc::StatusCode::INVALID_ARGUMENT,
                "invalid ONNX generation request"};

    GenerationRequest generation;
    generation.requestId = request->request_id();
    generation.requester = request->requester();
    generation.modelNode = request->model_node();
    generation.maxOutputTokens =
        static_cast<std::size_t>(request->max_output_tokens());
    generation.modelArtifact = request->model_artifact();
    generation.adapterArtifact = request->adapter_artifact();
    generation.modelChecksum = request->model_checksum();
    std::size_t promptBytes = 0;
    generation.messages.reserve(request->messages_size());
    for (const auto &message : request->messages())
    {
        if (!validRole(message.role()) ||
            message.content().size() > kMaxPromptBytes - promptBytes)
            return {grpc::StatusCode::INVALID_ARGUMENT,
                    "invalid ONNX prompt"};
        promptBytes += message.content().size();
        generation.messages.push_back({message.role(), message.content()});
    }

    auto state = std::make_shared<StreamState>(config_.maxBufferedTokens,
                                               config_.maxBufferedBytes);
    provider_->generate(
        generation,
        [state](std::string_view token) { state->push(token); },
        [state](GenerationCompletion completion)
        {
            state->complete(std::move(completion));
        },
        state->cancellation);

    for (;;)
    {
        std::string token;
        std::optional<GenerationCompletion> completion;
        bool overflowed = false;
        {
            std::unique_lock lock(state->mutex);
            state->changed.wait_for(lock, std::chrono::milliseconds(20), [&]
            {
                return !state->tokens.empty() || state->completion.has_value() ||
                       state->overflowed || context->IsCancelled();
            });
            if (context->IsCancelled())
            {
                state->closed = true;
                state->cancellation.cancel();
                state->changed.notify_all();
                return {grpc::StatusCode::CANCELLED, "client cancelled generation"};
            }
            overflowed = state->overflowed;
            if (!state->tokens.empty())
            {
                token = std::move(state->tokens.front());
                state->tokens.pop_front();
                state->bufferedBytes -= token.size();
                state->changed.notify_all();
            }
            else if (state->completion)
            {
                completion = std::move(*state->completion);
                state->closed = true;
                state->changed.notify_all();
            }
        }

        if (overflowed)
            return {grpc::StatusCode::RESOURCE_EXHAUSTED,
                    "inference stream buffer limit exceeded"};
        if (!token.empty())
        {
            webtest::rpc::v1::OnnxGenerateEvent event;
            event.set_token(std::move(token));
            if (!writer->Write(event))
            {
                std::lock_guard lock(state->mutex);
                state->closed = true;
                state->cancellation.cancel();
                state->changed.notify_all();
                return {grpc::StatusCode::CANCELLED,
                        "client stopped reading generation"};
            }
            continue;
        }
        if (completion)
        {
            webtest::rpc::v1::OnnxGenerateEvent event;
            auto *terminal = event.mutable_completed();
            terminal->set_success(completion->success);
            terminal->set_cancelled(completion->cancelled);
            terminal->set_error(std::move(completion->error));
            if (!writer->Write(event))
                return {grpc::StatusCode::CANCELLED,
                        "client stopped before completion"};
            return grpc::Status::OK;
        }
    }
}

grpc::Status GrpcOnnxWorkerService::Validate(
    grpc::ServerContext *context,
    const webtest::rpc::v1::OnnxValidateRequest *request,
    webtest::rpc::v1::OnnxValidateReply *reply)
{
    if (!context || !request || !reply)
        return {grpc::StatusCode::INTERNAL, "invalid RPC state"};
    if (!authenticated(*context))
        return {grpc::StatusCode::UNAUTHENTICATED, "invalid inference token"};
    if (request->model_artifact().empty() || request->model_checksum().empty())
        return {grpc::StatusCode::INVALID_ARGUMENT,
                "model artifact and checksum are required"};

    ModelVersion version;
    version.id = request->version_id();
    version.nodeId = request->node_id();
    // 独立进程内部最终仍调用进程内 Runtime；onnx-grpc 是聊天进程的路由名。
    version.runtime = "onnx-inprocess";
    version.modelArtifact = request->model_artifact();
    version.adapterArtifact = request->adapter_artifact();
    version.checksum = request->model_checksum();
    const auto validation = validator_->validateVersion(version);
    reply->set_valid(validation.valid);
    reply->set_error(validation.error);
    return grpc::Status::OK;
}

} // namespace webserver::phase11
