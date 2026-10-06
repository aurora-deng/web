#pragma once

#include "server/phase11/ai/ModelProvider/ModelProvider.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

namespace webserver::phase11
{

struct GrpcOnnxModelProviderConfig
{
    // 默认只连本机独立推理进程。跨主机部署时应同时配置 rootCertificates。
    std::string target{"127.0.0.1:50061"};
    std::string authenticationToken;
    // PEM CA 内容；为空时使用明文 channel，便于本机学习和测试。
    std::string rootCertificates;
    std::string tlsServerName;
    std::size_t workerCount = 2;
    std::size_t maxQueued = 32;
    std::size_t maxStreamBytes = 1024 * 1024;
    std::chrono::milliseconds rpcTimeout{std::chrono::minutes(5)};
    std::chrono::milliseconds validationTimeout{std::chrono::minutes(2)};
};

/**
 * 把独立 ONNX 推理进程的 gRPC server-streaming 接口适配回 IModelProvider。
 *
 * Reactor 只负责投递；阻塞的 ClientReader 位于有界 Worker。一个后台取消观察器
 * 统一扫描 CancellationToken 并调用 ClientContext::TryCancel，不为每个请求额外创建
 * 一条线程。远端崩溃只完成当前生成，不会终止聊天主进程。
 */
class GrpcOnnxModelProvider final : public IModelProvider,
                                    public IModelArtifactValidator
{
public:
    explicit GrpcOnnxModelProvider(GrpcOnnxModelProviderConfig config);
    ~GrpcOnnxModelProvider() override;

    GrpcOnnxModelProvider(const GrpcOnnxModelProvider &) = delete;
    GrpcOnnxModelProvider &operator=(const GrpcOnnxModelProvider &) = delete;

    [[nodiscard]] std::string name() const override { return "onnx-grpc"; }
    [[nodiscard]] bool ready() const noexcept override;
    [[nodiscard]] std::string supportedRuntime() const override
    {
        return "onnx-grpc";
    }
    [[nodiscard]] ModelArtifactValidation validateVersion(
        const ModelVersion &version) override;
    GenerationHandle generate(const GenerationRequest &request,
                              TokenSink onToken,
                              CompletionSink onComplete,
                              CancellationToken cancellation = {}) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace webserver::phase11
