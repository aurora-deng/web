#include "server/phase11/ai/InProcessOnnxModelProvider/InProcessOnnxModelProvider.h"

#include "server/phase11/runtime/BoundedExecutor/BoundedExecutor.h"
#include "server/phase11/transport/FlatJson/FlatJson.h"

#include <openssl/evp.h>
#include <ort_genai.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace webserver::phase11
{
namespace
{

constexpr std::string_view kChecksumPrefix = "sha256-tree-v1:";

void ensureGenAiRuntime()
{
    // OgaHandle 必须覆盖进程中所有 GenAI 对象的生命周期，而且只能存在一个。
    // 函数静态对象在 main 内 Provider 全部销毁后才执行 OgaShutdown。
    static OgaHandle handle;
    (void)handle;
}

bool isPathInside(const std::filesystem::path &root,
                  const std::filesystem::path &candidate)
{
    const auto mismatch = std::mismatch(root.begin(), root.end(),
                                        candidate.begin(), candidate.end());
    return mismatch.first == root.end();
}

std::filesystem::path resolveRelativeDirectory(
    const std::filesystem::path &root, const std::string &artifact)
{
    const std::filesystem::path relative(artifact);
    if (artifact.empty() || relative.is_absolute() || relative.has_root_path())
        throw std::invalid_argument("ONNX model artifact must be a relative directory");

    const auto resolved = std::filesystem::canonical(root / relative);
    if (!isPathInside(root, resolved) || !std::filesystem::is_directory(resolved))
        throw std::invalid_argument("ONNX model artifact escapes the configured root");
    if (!std::filesystem::is_regular_file(resolved / "genai_config.json"))
        throw std::invalid_argument("ONNX model directory has no genai_config.json");
    return resolved;
}

std::filesystem::path resolveAdapter(const std::filesystem::path &modelDir,
                                     const std::string &artifact)
{
    const std::filesystem::path relative(artifact);
    if (artifact.empty() || relative.is_absolute() || relative.has_root_path())
        throw std::invalid_argument("ONNX adapter artifact must be relative to its model");
    const auto resolved = std::filesystem::canonical(modelDir / relative);
    if (!isPathInside(modelDir, resolved) ||
        !std::filesystem::is_regular_file(resolved))
        throw std::invalid_argument("ONNX adapter escapes its model directory");
    return resolved;
}

void digestBytes(EVP_MD_CTX *context, const void *data, std::size_t size)
{
    if (EVP_DigestUpdate(context, data, size) != 1)
        throw std::runtime_error("cannot update ONNX artifact checksum");
}

void digestUint64(EVP_MD_CTX *context, std::uint64_t value)
{
    std::array<unsigned char, 8> encoded{};
    for (std::size_t index = 0; index < encoded.size(); ++index)
        encoded[encoded.size() - index - 1] =
            static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    digestBytes(context, encoded.data(), encoded.size());
}

std::string hexDigest(const unsigned char *bytes, unsigned int size)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.resize(static_cast<std::size_t>(size) * 2U);
    for (unsigned int index = 0; index < size; ++index)
    {
        result[index * 2U] = digits[bytes[index] >> 4U];
        result[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
    }
    return result;
}

std::string buildMessagesJson(const GenerationRequest &request,
                              std::size_t maxBytes)
{
    using transport::quoteJson;
    std::string json = "[";
    bool first = true;
    for (const auto &message : request.messages)
    {
        if (message.role != "system" && message.role != "user" &&
            message.role != "assistant" && message.role != "tool")
            throw std::invalid_argument("ONNX prompt contains an unsupported role");
        if (!first) json.push_back(',');
        first = false;
        json += "{\"role\":" + quoteJson(message.role) +
                ",\"content\":" + quoteJson(message.content) + "}";
        if (json.size() > maxBytes)
            throw std::invalid_argument("ONNX prompt exceeds configured limit");
    }
    json.push_back(']');
    if (json.size() > maxBytes)
        throw std::invalid_argument("ONNX prompt exceeds configured limit");
    return json;
}

std::string safeFailure(std::string prefix, std::string message,
                        const std::filesystem::path &modelRoot)
{
    const auto root = modelRoot.string();
    std::size_t position = 0;
    while (!root.empty() &&
           (position = message.find(root, position)) != std::string::npos)
    {
        message.replace(position, root.size(), "<model-root>");
        position += 12;
    }
    if (message.size() > 512)
        message.resize(512);
    return std::move(prefix) + ": " + message;
}

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
        // 回调属于上层边界，异常不能越过推理 Worker 导致 std::terminate。
    }
}

} // namespace

std::string computeOnnxArtifactChecksum(const std::filesystem::path &modelDir)
{
    const auto root = std::filesystem::canonical(modelDir);
    if (!std::filesystem::is_directory(root))
        throw std::invalid_argument("ONNX artifact root is not a directory");

    struct Entry final
    {
        std::string relative;
        std::filesystem::path absolute;
        std::uintmax_t size = 0;
    };
    std::vector<Entry> entries;
    for (const auto &item : std::filesystem::recursive_directory_iterator(root))
    {
        const auto status = item.symlink_status();
        if (std::filesystem::is_symlink(status))
            throw std::invalid_argument("ONNX artifact tree must not contain symlinks");
        if (std::filesystem::is_directory(status))
            continue;
        if (!std::filesystem::is_regular_file(status))
            throw std::invalid_argument("ONNX artifact tree contains a special file");
        entries.push_back({std::filesystem::relative(item.path(), root).generic_string(),
                           item.path(), item.file_size()});
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry &left, const Entry &right)
              { return left.relative < right.relative; });

    using DigestPointer = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    DigestPointer digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("cannot initialize ONNX artifact checksum");

    std::array<char, 64 * 1024> buffer{};
    for (const auto &entry : entries)
    {
        digestUint64(digest.get(), entry.relative.size());
        digestBytes(digest.get(), entry.relative.data(), entry.relative.size());
        digestUint64(digest.get(), entry.size);

        std::ifstream input(entry.absolute, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot read ONNX artifact file");
        std::uintmax_t consumed = 0;
        while (input)
        {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0)
            {
                digestBytes(digest.get(), buffer.data(),
                            static_cast<std::size_t>(count));
                consumed += static_cast<std::uintmax_t>(count);
            }
        }
        if (!input.eof() || consumed != entry.size)
            throw std::runtime_error("ONNX artifact changed while hashing");
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(digest.get(), bytes.data(), &size) != 1)
        throw std::runtime_error("cannot finalize ONNX artifact checksum");
    return std::string(kChecksumPrefix) + hexDigest(bytes.data(), size);
}

class InProcessOnnxModelProvider::Impl final
{
public:
    explicit Impl(InProcessOnnxModelProviderConfig config)
        : config_(std::move(config)),
          executor_(config_.workerCount, config_.maxQueued)
    {
        if (config_.workerCount == 0 || config_.maxQueued == 0 ||
            config_.maxLoadedModels == 0 ||
            config_.maxAdaptersPerModel == 0 || config_.maxPromptBytes == 0)
            throw std::invalid_argument("invalid ONNX provider limits");
        config_.modelRoot = std::filesystem::canonical(config_.modelRoot);
        if (!std::filesystem::is_directory(config_.modelRoot))
            throw std::invalid_argument("ONNX model root is not a directory");
        ensureGenAiRuntime();
    }

    ~Impl()
    {
        std::vector<CancellationToken> cancellations;
        {
            std::lock_guard lock(jobsMutex_);
            stopping_ = true;
            cancellations.reserve(jobs_.size());
            for (const auto &[id, job] : jobs_)
            {
                (void)id;
                cancellations.push_back(job->cancellation);
            }
        }
        for (const auto &cancellation : cancellations)
            cancellation.cancel();
        executor_.shutdown();
        // Worker 全部退出后才清理模型，保证 OgaModel/OgaTokenizer 不会悬空。
        std::lock_guard lock(modelsMutex_);
        models_.clear();
    }

    bool ready() const noexcept
    {
        std::lock_guard lock(jobsMutex_);
        return !stopping_ && executor_.accepting();
    }

    ModelArtifactValidation validateVersion(const ModelVersion &version)
    {
        try
        {
            if (version.runtime != "onnx-inprocess")
                throw std::invalid_argument(
                    "ONNX validator received a different runtime");
            if (config_.verifyArtifactChecksums &&
                version.checksum.rfind(kChecksumPrefix, 0) != 0)
                throw std::invalid_argument(
                    "ONNX model checksum must use sha256-tree-v1");

            GenerationRequest request;
            request.modelArtifact = version.modelArtifact;
            request.adapterArtifact = version.adapterArtifact;
            request.modelChecksum = version.checksum;
            const auto modelDir = resolveRelativeDirectory(
                config_.modelRoot, request.modelArtifact);
            const auto model = loadModel(request, modelDir);
            if (!request.adapterArtifact.empty())
            {
                const auto adapterPath = resolveAdapter(
                    modelDir, request.adapterArtifact);
                // LoadAdapter 在这里既解析 .onnx_adapter 的 FlatBuffers，也验证
                // 它能否挂到这个基础模型。试装成功前不会切换数据库 Active 指针。
                (void)loadAdapter(model, adapterPath);
            }
            return {true, {}};
        }
        catch (const std::exception &error)
        {
            return {false, safeFailure("ONNX artifact validation failed",
                                       error.what(), config_.modelRoot)};
        }
        catch (...)
        {
            return {false, "ONNX artifact validation failed: unknown error"};
        }
    }

    GenerationHandle generate(const GenerationRequest &request,
                              TokenSink onToken,
                              CompletionSink onComplete,
                              CancellationToken cancellation)
    {
        const GenerationHandle handle{request.requestId, cancellation};
        std::string validationError;
        if (request.requestId.empty())
            validationError = "ONNX request id is required";
        else if (request.modelArtifact.empty())
            validationError = "ONNX model artifact is required";
        else if (request.messages.empty())
            validationError = "ONNX prompt messages are required";
        else if (request.maxOutputTokens == 0 || request.maxOutputTokens > 4096)
            validationError = "ONNX output token limit is invalid";
        else if (!onComplete)
            validationError = "ONNX completion callback is required";
        else if (config_.verifyArtifactChecksums &&
                 request.modelChecksum.rfind(kChecksumPrefix, 0) != 0)
            validationError = "ONNX model checksum must use sha256-tree-v1";

        if (!validationError.empty())
        {
            completeSafely(onComplete, {false, cancellation.cancelled(),
                                        std::move(validationError), {}});
            return handle;
        }

        auto job = std::make_shared<Job>(Job{request, std::move(onToken),
                                             std::move(onComplete), cancellation});
        std::string rejected;
        {
            std::lock_guard lock(jobsMutex_);
            if (stopping_)
                rejected = "ONNX provider is stopping";
            else if (!jobs_.emplace(request.requestId, job).second)
                rejected = "duplicate ONNX request id";
        }
        if (!rejected.empty())
        {
            completeSafely(job->onComplete,
                           {false, cancellation.cancelled(),
                            std::move(rejected), {}});
            return handle;
        }

        if (!executor_.submit([this, job] { run(job); }))
        {
            {
                std::lock_guard lock(jobsMutex_);
                jobs_.erase(request.requestId);
            }
            completeSafely(job->onComplete,
                           {false, cancellation.cancelled(),
                            "ONNX provider queue is full or stopping", {}});
        }
        return handle;
    }

private:
    struct AdapterContext final
    {
        std::unique_ptr<OgaAdapters> adapters;
        std::string name;
    };

    struct ModelContext final
    {
        std::unique_ptr<OgaModel> model;
        std::unique_ptr<OgaTokenizer> tokenizer;
        std::mutex tokenizerMutex;
        std::mutex adaptersMutex;
        std::unordered_map<std::string, std::shared_ptr<AdapterContext>> adapters;
        std::uint64_t lastUse = 0;
    };

    struct Job final
    {
        GenerationRequest request;
        TokenSink onToken;
        CompletionSink onComplete;
        CancellationToken cancellation;
    };

    std::shared_ptr<ModelContext> loadModel(const GenerationRequest &request,
                                            const std::filesystem::path &modelDir)
    {
        const std::string key = modelDir.string() + '\n' + request.modelChecksum;
        std::lock_guard lock(modelsMutex_);
        if (const auto found = models_.find(key); found != models_.end())
        {
            found->second->lastUse = ++useClock_;
            return found->second;
        }

        if (models_.size() >= config_.maxLoadedModels)
        {
            auto victim = models_.end();
            for (auto current = models_.begin(); current != models_.end(); ++current)
                if (current->second.use_count() == 1 &&
                    (victim == models_.end() ||
                     current->second->lastUse < victim->second->lastUse))
                    victim = current;
            if (victim == models_.end())
                throw std::runtime_error("ONNX model cache is full");
            models_.erase(victim);
        }

        if (config_.verifyArtifactChecksums)
        {
            const auto actual = computeOnnxArtifactChecksum(modelDir);
            if (actual != request.modelChecksum)
                throw std::runtime_error("ONNX model checksum mismatch");
        }

        auto context = std::make_shared<ModelContext>();
        context->model = OgaModel::Create(modelDir.string().c_str());
        context->tokenizer = OgaTokenizer::Create(*context->model);
        context->lastUse = ++useClock_;
        models_.emplace(key, context);
        return context;
    }

    std::shared_ptr<AdapterContext> loadAdapter(
        const std::shared_ptr<ModelContext> &model,
        const std::filesystem::path &adapterPath)
    {
        const auto key = adapterPath.string();
        std::lock_guard lock(model->adaptersMutex);
        if (const auto found = model->adapters.find(key);
            found != model->adapters.end())
            return found->second;
        if (model->adapters.size() >= config_.maxAdaptersPerModel)
            throw std::runtime_error("ONNX adapter cache is full");

        auto context = std::make_shared<AdapterContext>();
        context->adapters = OgaAdapters::Create(*model->model);
        // 名字只在对应 OgaAdapters 内使用；一个容器只装一个 Adapter，避免并发装载
        // 与正在生成的请求互相修改同一 Adapter 集合。
        context->name = "phase11-adapter";
        context->adapters->LoadAdapter(adapterPath.string().c_str(),
                                       context->name.c_str());
        model->adapters.emplace(key, context);
        return context;
    }

    void finish(const std::shared_ptr<Job> &job,
                GenerationCompletion completion) noexcept
    {
        {
            std::lock_guard lock(jobsMutex_);
            jobs_.erase(job->request.requestId);
        }
        completeSafely(job->onComplete, std::move(completion));
    }

    void run(const std::shared_ptr<Job> &job) noexcept
    {
        try
        {
            if (job->cancellation.cancelled())
            {
                finish(job, {false, true, "ONNX generation cancelled", {}});
                return;
            }

            const auto modelDir = resolveRelativeDirectory(
                config_.modelRoot, job->request.modelArtifact);
            const auto model = loadModel(job->request, modelDir);
            if (job->cancellation.cancelled())
            {
                finish(job, {false, true, "ONNX generation cancelled", {}});
                return;
            }

            const auto messages = buildMessagesJson(job->request,
                                                     config_.maxPromptBytes);
            std::string prompt;
            std::unique_ptr<OgaSequences> sequences;
            std::unique_ptr<OgaTokenizerStream> tokenStream;
            {
                // Tokenizer 的文档没有承诺同一个实例可被多个线程同时调用；这里只锁住
                // 短暂的模板、编码和 Stream 创建，不锁住后续模型推理。
                std::lock_guard lock(model->tokenizerMutex);
                const auto formatted = model->tokenizer->ApplyChatTemplate(
                    nullptr, messages.c_str(), nullptr, true);
                prompt = static_cast<const char *>(formatted);
                if (prompt.size() > config_.maxPromptBytes)
                    throw std::runtime_error("formatted ONNX prompt is too large");
                sequences = OgaSequences::Create();
                model->tokenizer->Encode(prompt.c_str(), *sequences);
                tokenStream = OgaTokenizerStream::Create(*model->tokenizer);
            }
            if (sequences->Count() != 1)
                throw std::runtime_error("ONNX tokenizer returned an invalid batch");
            const auto promptTokens = sequences->SequenceCount(0);
            if (job->request.maxOutputTokens >
                std::numeric_limits<std::size_t>::max() - promptTokens)
                throw std::runtime_error("ONNX token limit overflow");

            auto params = OgaGeneratorParams::Create(*model->model);
            params->SetSearchOption("batch_size", 1);
            params->SetSearchOption(
                "max_length",
                static_cast<double>(promptTokens + job->request.maxOutputTokens));
            auto generator = OgaGenerator::Create(*model->model, *params);

            std::shared_ptr<AdapterContext> adapter;
            if (!job->request.adapterArtifact.empty())
            {
                const auto adapterPath = resolveAdapter(
                    modelDir, job->request.adapterArtifact);
                adapter = loadAdapter(model, adapterPath);
                generator->SetActiveAdapter(*adapter->adapters,
                                            adapter->name.c_str());
            }
            generator->AppendTokenSequences(*sequences);

            std::size_t generated = 0;
            while (!generator->IsDone() &&
                   generated < job->request.maxOutputTokens)
            {
                if (job->cancellation.cancelled())
                {
                    finish(job, {false, true, "ONNX generation cancelled", {}});
                    return;
                }
                generator->GenerateNextToken();
                const auto next = generator->GetNextTokens();
                if (next.empty() && !generator->IsDone())
                    throw std::runtime_error("ONNX generator made no progress");
                for (const auto token : next)
                {
                    if (generated >= job->request.maxOutputTokens)
                        break;
                    ++generated;
                    const char *piece = tokenStream->Decode(token);
                    if (piece && *piece && job->onToken)
                        job->onToken(piece);
                }
                if (generator->IsSessionTerminated())
                    throw std::runtime_error("ONNX Runtime terminated the session");
            }

            if (job->cancellation.cancelled())
                finish(job, {false, true, "ONNX generation cancelled", {}});
            else
                finish(job, {true, false, {}, {}});
        }
        catch (const std::exception &failure)
        {
            finish(job, {false, job->cancellation.cancelled(),
                         job->cancellation.cancelled()
                             ? "ONNX generation cancelled"
                             : safeFailure("ONNX generation failed",
                                           failure.what(), config_.modelRoot),
                         {}});
        }
        catch (...)
        {
            finish(job, {false, job->cancellation.cancelled(),
                         job->cancellation.cancelled()
                             ? "ONNX generation cancelled"
                             : "ONNX generation failed",
                         {}});
        }
    }

    InProcessOnnxModelProviderConfig config_;
    BoundedExecutor executor_;

    mutable std::mutex jobsMutex_;
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs_;
    bool stopping_ = false;

    std::mutex modelsMutex_;
    std::unordered_map<std::string, std::shared_ptr<ModelContext>> models_;
    std::uint64_t useClock_ = 0;
};

InProcessOnnxModelProvider::InProcessOnnxModelProvider(
    InProcessOnnxModelProviderConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

InProcessOnnxModelProvider::~InProcessOnnxModelProvider() = default;

bool InProcessOnnxModelProvider::ready() const noexcept
{
    return impl_->ready();
}

ModelArtifactValidation InProcessOnnxModelProvider::validateVersion(
    const ModelVersion &version)
{
    return impl_->validateVersion(version);
}

GenerationHandle InProcessOnnxModelProvider::generate(
    const GenerationRequest &request, TokenSink onToken,
    CompletionSink onComplete, CancellationToken cancellation)
{
    return impl_->generate(request, std::move(onToken), std::move(onComplete),
                           std::move(cancellation));
}

} // namespace webserver::phase11
