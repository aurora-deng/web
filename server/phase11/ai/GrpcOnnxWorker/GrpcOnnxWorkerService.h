#pragma once

#include "server/phase11/ai/ModelProvider/ModelProvider.h"
#include "web_learning.grpc.pb.h"

#include <cstddef>
#include <memory>
#include <string>

namespace webserver::phase11
{

struct GrpcOnnxWorkerServiceConfig
{
    std::string authenticationToken;
    // 慢客户端最多让这些 Token/字节停在模型进程内；达到上限后推理 Worker
    // 等待 gRPC 消费，从而把 HTTP/2 背压传回模型生成循环。
    std::size_t maxBufferedTokens = 256;
    std::size_t maxBufferedBytes = 256 * 1024;
};

/** 独立推理进程的同步 gRPC 外壳，真正生成仍由 IModelProvider 的有界 Worker 执行。 */
class GrpcOnnxWorkerService final
    : public webtest::rpc::v1::OnnxInferenceService::Service
{
public:
    GrpcOnnxWorkerService(std::shared_ptr<IModelProvider> provider,
                          std::shared_ptr<IModelArtifactValidator> validator,
                          GrpcOnnxWorkerServiceConfig config = {});

    grpc::Status Generate(
        grpc::ServerContext *context,
        const webtest::rpc::v1::OnnxGenerateRequest *request,
        grpc::ServerWriter<webtest::rpc::v1::OnnxGenerateEvent> *writer) override;
    grpc::Status Validate(
        grpc::ServerContext *context,
        const webtest::rpc::v1::OnnxValidateRequest *request,
        webtest::rpc::v1::OnnxValidateReply *reply) override;

private:
    [[nodiscard]] bool authenticated(grpc::ServerContext &context) const noexcept;

    std::shared_ptr<IModelProvider> provider_;
    std::shared_ptr<IModelArtifactValidator> validator_;
    GrpcOnnxWorkerServiceConfig config_;
};

} // namespace webserver::phase11
