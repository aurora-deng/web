#include "server/phase11/ai/ModelRegistryService/ModelRegistryService.h"

#include <filesystem>
#include <set>

namespace webserver::phase11
{

ModelNode ModelRegistryService::createNode(std::string name,
                                           ModelNodeKind kind,
                                           TimePoint now)
{
    if (name.empty() || name.size() > 80)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid model node name");
    return models_.createModelNode({0, std::move(name), kind, {}, now});
}

ModelVersion ModelRegistryService::registerVersion(
    ModelNodeId node, std::string runtime, std::string modelArtifact,
    std::string adapterArtifact, std::string checksum, TimePoint now)
{
    if (!models_.findModelNode(node))
        throw ApplicationError(ErrorCode::NotFound, "model node not found");
    if (runtime != "ollama" && runtime != "onnx-inprocess" &&
        runtime != "onnx-grpc")
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "unsupported model runtime");
    if (modelArtifact.empty() || checksum.empty())
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "model artifact and checksum are required");
    if (!adapterArtifact.empty())
    {
        if (runtime != "onnx-inprocess" && runtime != "onnx-grpc")
            throw ApplicationError(
                ErrorCode::InvalidArgument,
                "adapter artifacts are only supported by ONNX runtimes");
        const std::filesystem::path adapterPath(adapterArtifact);
        if (adapterPath.is_absolute() || adapterPath.has_root_path() ||
            adapterPath.extension() != ".onnx_adapter")
            throw ApplicationError(
                ErrorCode::InvalidArgument,
                "ONNX adapter must be a relative .onnx_adapter artifact");
    }
    return models_.createModelVersion(
        {0, node, std::move(runtime), std::move(modelArtifact),
         std::move(adapterArtifact), std::move(checksum),
         ModelVersionState::Candidate, now});
}

bool ModelRegistryService::createsParentCycle(ModelNodeId from,
                                              ModelNodeId to) const
{
    // 新边 from -> to。若从 to 沿现有父子边能够回到 from，就会形成环。
    std::set<ModelNodeId> frontier{to};
    std::set<ModelNodeId> visited;
    const auto edges = models_.modelEdges();
    while (!frontier.empty())
    {
        const auto current = *frontier.begin();
        frontier.erase(frontier.begin());
        if (current == from)
            return true;
        if (!visited.insert(current).second)
            continue;
        for (const auto &edge : edges)
            if (edge.kind == ModelEdgeKind::ParentControlsChild &&
                edge.from == current)
                frontier.insert(edge.to);
    }
    return false;
}

ModelEdge ModelRegistryService::connect(ModelNodeId from, ModelNodeId to,
                                        ModelEdgeKind kind)
{
    if (!models_.findModelNode(from) || !models_.findModelNode(to))
        throw ApplicationError(ErrorCode::NotFound, "model node not found");
    if (from == to)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "model cannot connect to itself");
    for (const auto &edge : models_.modelEdges())
        if (edge.from == from && edge.to == to && edge.kind == kind)
            throw ApplicationError(ErrorCode::Conflict,
                                   "model edge already exists");
    if (kind == ModelEdgeKind::ParentControlsChild &&
        createsParentCycle(from, to))
        throw ApplicationError(ErrorCode::Conflict,
                               "parent relation would create a cycle");
    return models_.createModelEdge({0, from, to, kind});
}

void ModelRegistryService::bindAiAccount(UserId aiUser, ModelNodeId node)
{
    if (!models_.bindAiAccount({aiUser, node}))
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "AI account or model node is invalid");
}

void ModelRegistryService::registerValidator(
    std::shared_ptr<IModelArtifactValidator> validator)
{
    if (!validator || validator->supportedRuntime().empty())
        throw std::invalid_argument("model artifact validator is required");
    const auto runtime = validator->supportedRuntime();
    std::lock_guard lock(validatorsMutex_);
    validators_.insert_or_assign(runtime, std::move(validator));
}

void ModelRegistryService::activate(ModelNodeId node, ModelVersionId version)
{
    validateAndActivate(node, version, false);
}

void ModelRegistryService::rollback(ModelNodeId node,
                                    ModelVersionId retiredVersion)
{
    validateAndActivate(node, retiredVersion, true);
}

void ModelRegistryService::validateAndActivate(ModelNodeId node,
                                               ModelVersionId version,
                                               bool rollbackOnly)
{
    const auto modelNode = models_.findModelNode(node);
    const auto candidate = models_.findModelVersion(version);
    if (!modelNode || !candidate || candidate->nodeId != node)
        throw ApplicationError(ErrorCode::NotFound,
                               "model node or version not found");
    if (candidate->state == ModelVersionState::Rejected)
        throw ApplicationError(ErrorCode::Conflict,
                               "rejected model version cannot be activated");
    if (rollbackOnly && candidate->state != ModelVersionState::Retired)
        throw ApplicationError(ErrorCode::Conflict,
                               "rollback target must be a retired version");

    std::shared_ptr<IModelArtifactValidator> validator;
    {
        std::lock_guard lock(validatorsMutex_);
        const auto found = validators_.find(candidate->runtime);
        if (found != validators_.end())
            validator = found->second;
    }
    if (!validator)
        throw ApplicationError(ErrorCode::Unavailable,
                               "model runtime validator is not registered");

    // 这里先试装，Repository 只有在全部检查成功后才会移动 Active 指针；
    // 因此校验异常或 Adapter 不兼容时，旧模型仍保持 Active。
    const auto validation = validator->validateVersion(*candidate);
    if (!validation.valid)
    {
        auto message = validation.error.empty()
                           ? std::string("model artifact validation failed")
                           : validation.error;
        if (message.size() > 512)
            message.resize(512);
        throw ApplicationError(
            ErrorCode::Conflict, std::move(message));
    }
    if (!models_.activateModelVersion(node, version))
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "model version cannot be activated");
}

void ModelRouter::registerProvider(std::shared_ptr<IModelProvider> provider)
{
    if (!provider || provider->name().empty())
        throw std::invalid_argument("model provider and name are required");
    std::lock_guard lock(mutex_);
    providers_.insert_or_assign(provider->name(), std::move(provider));
}

bool ModelRouter::modelReady(UserId aiUser) const
{
    const auto binding = models_.findModelBinding(aiUser);
    if (!binding)
        return false;
    const auto node = models_.findModelNode(binding->nodeId);
    if (!node || !node->activeVersionId)
        return false;
    const auto version = models_.findModelVersion(*node->activeVersionId);
    if (!version)
        return false;
    std::lock_guard lock(mutex_);
    const auto provider = providers_.find(version->runtime);
    return provider != providers_.end() && provider->second->ready();
}

GenerationHandle ModelRouter::generate(UserId aiUser,
                                       GenerationRequest request,
                                       TokenSink onToken,
                                       CompletionSink onComplete,
                                       CancellationToken cancellation)
{
    const auto binding = models_.findModelBinding(aiUser);
    if (!binding)
        throw ApplicationError(ErrorCode::NotFound,
                               "AI account has no model binding");
    const auto node = models_.findModelNode(binding->nodeId);
    if (!node || !node->activeVersionId)
        throw ApplicationError(ErrorCode::Unavailable,
                               "AI model has no active version");
    const auto version = models_.findModelVersion(*node->activeVersionId);
    if (!version)
        throw ApplicationError(ErrorCode::Unavailable,
                               "active model version is missing");

    std::shared_ptr<IModelProvider> provider;
    {
        std::lock_guard lock(mutex_);
        const auto found = providers_.find(version->runtime);
        if (found != providers_.end())
            provider = found->second;
    }
    if (!provider || !provider->ready())
        throw ApplicationError(ErrorCode::Unavailable,
                               "selected model provider is not ready");
    if (request.requestId.empty() || request.messages.empty() ||
        request.maxOutputTokens == 0 || request.maxOutputTokens > 4096)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid generation request");

    request.modelNode = node->id;
    request.modelArtifact = version->modelArtifact;
    request.adapterArtifact = version->adapterArtifact;
    request.modelChecksum = version->checksum;
    const MessageModelTrace trace{
        node->id, version->id, version->adapterArtifact};
    return provider->generate(
        request, std::move(onToken),
        [trace, completion = std::move(onComplete)](
            GenerationCompletion result) mutable {
            result.trace = trace;
            completion(std::move(result));
        },
        std::move(cancellation));
}

} // namespace webserver::phase11
