#pragma once

#include "server/phase11/domain/Domain/Domain.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace webserver::phase11
{

struct PromptMessage
{
    std::string role;
    std::string content;
};

struct GenerationRequest
{
    std::string requestId;
    UserId requester = 0;
    ModelNodeId modelNode = 0;
    std::vector<PromptMessage> messages;
    std::size_t maxOutputTokens = 512;
    // ModelRouter 根据当前激活版本填入。Provider 不访问 Repository，因而模型档案
    // 与具体网络/推理实现仍保持解耦。
    std::string modelArtifact;
    std::string adapterArtifact;
    // 本地模型 Provider 用它验证完整模型目录；远程 Provider 可以忽略它。
    std::string modelChecksum;
};

struct GenerationCompletion
{
    bool success = false;
    bool cancelled = false;
    std::string error;
    MessageModelTrace trace;
};

struct ModelArtifactValidation
{
    bool valid = false;
    // 该信息面向部署/管理端，不返回给普通聊天用户。实现仍应删除绝对路径、
    // 密钥等敏感内容，并限制长度。
    std::string error;
};

/**
 * 模型版本启用前的“试装”端口。
 *
 * ModelRegistryService 只认识这个抽象接口，不认识 OgaModel、HTTP 客户端或
 * gRPC Stub。具体 Provider 负责验证自己的模型文件、校验和和 Adapter 兼容性。
 * validateVersion 可能加载数 GB 模型，因此管理端必须在线程池执行，不能在
 * Reactor 线程中直接调用。
 */
class IModelArtifactValidator
{
public:
    virtual ~IModelArtifactValidator() = default;
    [[nodiscard]] virtual std::string supportedRuntime() const = 0;
    [[nodiscard]] virtual ModelArtifactValidation validateVersion(
        const ModelVersion &version) = 0;
};

class CancellationToken
{
public:
    CancellationToken() : cancelled_(std::make_shared<std::atomic_bool>(false)) {}
    void cancel() const noexcept { cancelled_->store(true, std::memory_order_release); }
    [[nodiscard]] bool cancelled() const noexcept
    {
        return cancelled_->load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<std::atomic_bool> cancelled_;
};

struct GenerationHandle
{
    std::string requestId;
    CancellationToken cancellation;
};

using TokenSink = std::function<void(std::string_view)>;
using CompletionSink = std::function<void(GenerationCompletion)>;

class IModelProvider
{
public:
    virtual ~IModelProvider() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual bool ready() const noexcept = 0;
    virtual GenerationHandle generate(const GenerationRequest &request,
                                      TokenSink onToken,
                                      CompletionSink onComplete,
                                      CancellationToken cancellation) = 0;
};

/**
 * 依赖尚未安装时使用的显式占位 Provider。它不会伪造模型回答，而是立即返回
 * Unavailable，使普通聊天可以工作，同时让 AI 路由准确暴露“模型尚未配置”。
 */
class UnavailableModelProvider final : public IModelProvider
{
public:
    explicit UnavailableModelProvider(std::string providerName,
                                      std::string reason)
        : name_(std::move(providerName)), reason_(std::move(reason))
    {
    }

    std::string name() const override { return name_; }
    bool ready() const noexcept override { return false; }

    GenerationHandle generate(const GenerationRequest &request,
                              TokenSink,
                              CompletionSink onComplete,
                              CancellationToken cancellation) override
    {
        onComplete({false, cancellation.cancelled(), reason_, {}});
        return {request.requestId, std::move(cancellation)};
    }

private:
    std::string name_;
    std::string reason_;
};

} // namespace webserver::phase11
