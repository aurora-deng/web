#include "backend/ai/bootstrap/ModelProviderAssembly/ModelProviderAssembly.h"
#include "backend/application/config/RuntimeConfig/RuntimeConfig.h"
#ifdef WEBSERVER_HAS_CURL_CLIENT
#include "backend/ai/models/OllamaModelProvider/OllamaModelProvider.h"
#endif
#ifdef WEBSERVER_HAS_ONNX_GENAI
#include "backend/ai/models/InProcessOnnxModelProvider/InProcessOnnxModelProvider.h"
#endif
#ifdef WEBSERVER_HAS_PHASE11_GRPC_ONNX
#include "backend/ai/models/GrpcOnnxModelProvider/GrpcOnnxModelProvider.h"
#endif
#include <fstream>
#include <iterator>
#include <iostream>
#include <sys/stat.h>

namespace webserver::phase11 {
namespace {
std::size_t boundedEnvironmentSize(const char*n,std::size_t fallback,std::size_t low,std::size_t high){return webserver::config::RuntimeConfig::size(n,fallback,low,high);}
#ifdef WEBSERVER_HAS_PHASE11_GRPC_ONNX
std::string readPhase11RegularFile(const char *name, const char *path)
{
    if (!path || *path == '\0')
        throw std::invalid_argument(std::string(name) + " is required");
    struct stat metadata{};
    if (::stat(path, &metadata) != 0 || !S_ISREG(metadata.st_mode))
        throw std::invalid_argument(std::string(name) +
                                    " must name a readable regular file");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::invalid_argument(std::string("cannot read ") + name);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}
#endif
}
void configureModelProviders(ModelRouter &router, ModelRegistryService &registry, std::vector<std::shared_ptr<IModelProvider>> &providers, std::vector<std::shared_ptr<ResilientModelProvider>> &resilient, bool productionMode) {
            webserver::phase11::ModelProviderResilienceConfig
                modelResilienceConfig;
            modelResilienceConfig.failureThreshold = boundedEnvironmentSize(
                "WEB_PHASE11_AI_CIRCUIT_FAILURES", 3, 1, 100);
            modelResilienceConfig.circuitOpenFor = std::chrono::milliseconds(
                boundedEnvironmentSize("WEB_PHASE11_AI_CIRCUIT_OPEN_MS",
                                       30'000, 100, 3'600'000));
            modelResilienceConfig.attemptTimeout = std::chrono::milliseconds(
                boundedEnvironmentSize("WEB_PHASE11_AI_PROVIDER_TIMEOUT_MS",
                                       300'000, 100, 3'600'000));
#ifdef WEBSERVER_HAS_CURL_CLIENT
            if (const char *ollamaUrl = std::getenv("WEB_PHASE11_OLLAMA_URL");
                ollamaUrl && *ollamaUrl)
            {
                webserver::phase11::OllamaModelProviderConfig ollamaConfig;
                ollamaConfig.baseUrl = ollamaUrl;
                ollamaConfig.maxConcurrent = boundedEnvironmentSize(
                    "WEB_PHASE11_OLLAMA_CONCURRENT", 4, 1, 64);
                ollamaConfig.maxQueued = boundedEnvironmentSize(
                    "WEB_PHASE11_OLLAMA_QUEUE", 64, 1, 65'536);
                ollamaConfig.connectTimeout = std::chrono::milliseconds(
                    boundedEnvironmentSize("WEB_PHASE11_OLLAMA_CONNECT_MS",
                                           2'000, 10, 60'000));
                ollamaConfig.requestTimeout = std::chrono::milliseconds(
                    boundedEnvironmentSize("WEB_PHASE11_OLLAMA_REQUEST_MS",
                                           120'000, 100, 3'600'000));
                auto primary = std::make_shared<
                    webserver::phase11::OllamaModelProvider>(
                        ollamaConfig);
                std::shared_ptr<webserver::phase11::IModelProvider> fallback;
                if (const char *fallbackUrl =
                        std::getenv("WEB_PHASE11_OLLAMA_FALLBACK_URL");
                    fallbackUrl && *fallbackUrl)
                {
                    auto fallbackConfig = ollamaConfig;
                    fallbackConfig.baseUrl = fallbackUrl;
                    fallback = std::make_shared<
                        webserver::phase11::OllamaModelProvider>(
                            std::move(fallbackConfig));
                }
                auto provider = std::make_shared<
                    webserver::phase11::ResilientModelProvider>(
                        std::move(primary), std::move(fallback),
                        modelResilienceConfig);
                router.registerProvider(provider);
                resilient.push_back(provider);
                providers.push_back(std::move(provider));
                std::cout << "Phase 11 Ollama provider enabled"
                          << (std::getenv("WEB_PHASE11_OLLAMA_FALLBACK_URL")
                                  ? " with fallback endpoint\n"
                                  : "\n");
            }
#else
            if (const char *ollamaUrl = std::getenv("WEB_PHASE11_OLLAMA_URL");
                ollamaUrl && *ollamaUrl)
                throw std::invalid_argument(
                    "WEB_PHASE11_OLLAMA_URL requires WEBSERVER_ENABLE_CURL_CLIENT");
#endif
#ifdef WEBSERVER_HAS_ONNX_GENAI
            if (const char *modelRoot =
                    std::getenv("WEB_PHASE11_ONNX_MODEL_ROOT");
                modelRoot && *modelRoot)
            {
                webserver::phase11::InProcessOnnxModelProviderConfig onnxConfig;
                onnxConfig.modelRoot = modelRoot;
                onnxConfig.workerCount = boundedEnvironmentSize(
                    "WEB_PHASE11_ONNX_WORKERS", 1, 1, 16);
                onnxConfig.maxQueued = boundedEnvironmentSize(
                    "WEB_PHASE11_ONNX_QUEUE", 8, 1, 1024);
                onnxConfig.maxLoadedModels = boundedEnvironmentSize(
                    "WEB_PHASE11_ONNX_MODELS", 2, 1, 32);
                onnxConfig.maxAdaptersPerModel = boundedEnvironmentSize(
                    "WEB_PHASE11_ONNX_ADAPTERS", 8, 1, 128);
                onnxConfig.maxPromptBytes = boundedEnvironmentSize(
                    "WEB_PHASE11_ONNX_PROMPT_BYTES", 1024 * 1024,
                    1024, 16 * 1024 * 1024);
                auto primary = std::make_shared<
                    webserver::phase11::InProcessOnnxModelProvider>(
                        std::move(onnxConfig));
                registry.registerValidator(primary);
                auto provider = std::make_shared<
                    webserver::phase11::ResilientModelProvider>(
                        std::move(primary), nullptr, modelResilienceConfig);
                router.registerProvider(provider);
                resilient.push_back(provider);
                providers.push_back(std::move(provider));
                std::cout << "Phase 11 in-process ONNX provider enabled\n";
            }
#else
            if (const char *modelRoot =
                    std::getenv("WEB_PHASE11_ONNX_MODEL_ROOT");
                modelRoot && *modelRoot)
                throw std::invalid_argument(
                    "WEB_PHASE11_ONNX_MODEL_ROOT requires WEBSERVER_ENABLE_ONNX_GENAI");
#endif
#ifdef WEBSERVER_HAS_PHASE11_GRPC_ONNX
            if (const char *target =
                    std::getenv("WEB_PHASE11_ONNX_GRPC_TARGET");
                target && *target)
            {
                webserver::phase11::GrpcOnnxModelProviderConfig grpcOnnxConfig;
                grpcOnnxConfig.target = target;
                if (const char *token =
                        std::getenv("WEB_PHASE11_ONNX_RPC_TOKEN"))
                    grpcOnnxConfig.authenticationToken = token;
                if (productionMode &&
                    grpcOnnxConfig.authenticationToken.size() < 32)
                    throw std::invalid_argument(
                        "production remote ONNX requires WEB_PHASE11_ONNX_RPC_TOKEN with at least 32 bytes");
                if (const char *ca =
                        std::getenv("WEB_PHASE11_ONNX_GRPC_TLS_CA"))
                    grpcOnnxConfig.rootCertificates = readPhase11RegularFile(
                        "WEB_PHASE11_ONNX_GRPC_TLS_CA", ca);
                else if (productionMode)
                    throw std::invalid_argument(
                        "production remote ONNX requires WEB_PHASE11_ONNX_GRPC_TLS_CA");
                if (const char *name =
                        std::getenv("WEB_PHASE11_ONNX_GRPC_TLS_NAME"))
                    grpcOnnxConfig.tlsServerName = name;
                grpcOnnxConfig.workerCount = boundedEnvironmentSize(
                    "WEB_PHASE11_ONNX_GRPC_WORKERS", 2, 1, 32);
                grpcOnnxConfig.maxQueued = boundedEnvironmentSize(
                    "WEB_PHASE11_ONNX_GRPC_QUEUE", 32, 1, 4096);
                grpcOnnxConfig.maxStreamBytes = boundedEnvironmentSize(
                    "WEB_PHASE11_ONNX_GRPC_STREAM_BYTES", 1024 * 1024,
                    1024, 16 * 1024 * 1024);
                grpcOnnxConfig.rpcTimeout = std::chrono::milliseconds(
                    boundedEnvironmentSize("WEB_PHASE11_ONNX_GRPC_RPC_MS",
                                           300'000, 100, 3'600'000));
                grpcOnnxConfig.validationTimeout = std::chrono::milliseconds(
                    boundedEnvironmentSize("WEB_PHASE11_ONNX_GRPC_VALIDATE_MS",
                                           120'000, 100, 3'600'000));
                auto primary = std::make_shared<
                    webserver::phase11::GrpcOnnxModelProvider>(
                        std::move(grpcOnnxConfig));
                registry.registerValidator(primary);
                auto provider = std::make_shared<
                    webserver::phase11::ResilientModelProvider>(
                        std::move(primary), nullptr, modelResilienceConfig);
                router.registerProvider(provider);
                resilient.push_back(provider);
                providers.push_back(std::move(provider));
                std::cout << "Phase 11 remote ONNX provider enabled\n";
            }
#else
            if (const char *target =
                    std::getenv("WEB_PHASE11_ONNX_GRPC_TARGET");
                target && *target)
                throw std::invalid_argument(
                    "WEB_PHASE11_ONNX_GRPC_TARGET requires gRPC and ONNX GenAI");
#endif

}
}
