#pragma once

#include "server/phase11/ai/ModelProvider/ModelProvider.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

namespace webserver::phase11
{

struct InProcessOnnxModelProviderConfig
{
    // 数据库只保存相对此目录的模型目录，避免数据库内容把服务器带到任意路径。
    std::filesystem::path modelRoot;
    std::size_t workerCount = 1;
    std::size_t maxQueued = 8;
    std::size_t maxLoadedModels = 2;
    std::size_t maxAdaptersPerModel = 8;
    std::size_t maxPromptBytes = 1024 * 1024;
    bool verifyArtifactChecksums = true;
};

/**
 * 计算本项目定义的模型目录摘要。
 *
 * 格式为 sha256-tree-v1:<hex>。摘要覆盖按相对路径排序后的所有普通文件，
 * 同时写入路径长度、路径、文件大小和文件内容；符号链接会被拒绝。配套 Python
 * 工具 scripts/phase11/model_checksum.py 使用完全相同的算法。
 */
std::string computeOnnxArtifactChecksum(const std::filesystem::path &modelDir);

/**
 * ONNX Runtime GenAI 的进程内 Provider。
 *
 * Reactor 只调用 generate 投递任务；模型加载、Tokenizer 和逐 Token 推理全部在
 * 独立有界 Worker 中执行。可以把它看成服务器后面的模型机房：前台把号码牌放进
 * 有上限的队列，机房生成一个 Token 就送回一个 Token，前台不会被推理占住。
 */
class InProcessOnnxModelProvider final : public IModelProvider,
                                         public IModelArtifactValidator
{
public:
    explicit InProcessOnnxModelProvider(InProcessOnnxModelProviderConfig config);
    ~InProcessOnnxModelProvider() override;

    InProcessOnnxModelProvider(const InProcessOnnxModelProvider &) = delete;
    InProcessOnnxModelProvider &operator=(const InProcessOnnxModelProvider &) = delete;

    [[nodiscard]] std::string name() const override { return "onnx-inprocess"; }
    [[nodiscard]] bool ready() const noexcept override;
    [[nodiscard]] std::string supportedRuntime() const override
    {
        return "onnx-inprocess";
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
