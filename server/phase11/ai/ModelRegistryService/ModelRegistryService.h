#pragma once

#include "server/phase11/ai/ModelProvider/ModelProvider.h"
#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/ports/Ports/Ports.h"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace webserver::phase11
{

class ModelRegistryService
{
public:
    explicit ModelRegistryService(IModelRepository &models) : models_(models) {}

    ModelNode createNode(std::string name, ModelNodeKind kind, TimePoint now);
    ModelVersion registerVersion(ModelNodeId node, std::string runtime,
                                 std::string modelArtifact,
                                 std::string adapterArtifact,
                                 std::string checksum, TimePoint now);
    ModelEdge connect(ModelNodeId from, ModelNodeId to,
                      ModelEdgeKind kind);
    void bindAiAccount(UserId aiUser, ModelNodeId node);
    void registerValidator(std::shared_ptr<IModelArtifactValidator> validator);
    /**
     * 先让对应 Runtime 试装模型/Adapter，成功后才切换 Active 指针。
     * 调用可能很慢，必须由管理端 Worker 执行，不能占用 Reactor。
     */
    void activate(ModelNodeId node, ModelVersionId version);
    /** 重新启用该节点的一个 Retired 版本；目标仍需再次通过完整校验。 */
    void rollback(ModelNodeId node, ModelVersionId retiredVersion);

private:
    bool createsParentCycle(ModelNodeId from, ModelNodeId to) const;
    void validateAndActivate(ModelNodeId node, ModelVersionId version,
                             bool rollbackOnly);

    IModelRepository &models_;
    mutable std::mutex validatorsMutex_;
    std::unordered_map<std::string, std::shared_ptr<IModelArtifactValidator>>
        validators_;
};

/**
 * ModelRegistryService 管“模型档案”，ModelRouter 管“这次请求找哪台发动机”。
 * 分开后，数据库不会知道 libcurl/ONNX，Provider 也不需要知道模型树如何存储。
 */
class ModelRouter
{
public:
    explicit ModelRouter(IModelRepository &models) : models_(models) {}

    void registerProvider(std::shared_ptr<IModelProvider> provider);
    [[nodiscard]] bool modelReady(UserId aiUser) const;
    GenerationHandle generate(UserId aiUser, GenerationRequest request,
                              TokenSink onToken,
                              CompletionSink onComplete,
                              CancellationToken cancellation = {});

private:
    IModelRepository &models_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<IModelProvider>> providers_;
};

} // namespace webserver::phase11
