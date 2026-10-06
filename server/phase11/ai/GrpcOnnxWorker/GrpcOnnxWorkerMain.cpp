#include "server/phase11/ai/GrpcOnnxWorker/GrpcOnnxWorkerService.h"
#include "server/phase11/ai/InProcessOnnxModelProvider/InProcessOnnxModelProvider.h"

#include <grpcpp/grpcpp.h>

#include <pthread.h>
#include <signal.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{

std::size_t environmentSize(const char *name, std::size_t fallback,
                            std::size_t minimum, std::size_t maximum)
{
    const char *raw = std::getenv(name);
    if (!raw) return fallback;
    char *end = nullptr;
    errno = 0;
    const auto value = std::strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' ||
        value < minimum || value > maximum)
        throw std::invalid_argument(std::string(name) +
                                    " is outside the allowed range");
    return static_cast<std::size_t>(value);
}

bool environmentFlag(const char *name)
{
    const char *raw = std::getenv(name);
    return raw && (std::string(raw) == "1" || std::string(raw) == "true" ||
                   std::string(raw) == "TRUE");
}

std::string readRegularFile(const char *name, const char *path)
{
    if (!path || !*path || !std::filesystem::is_regular_file(path))
        throw std::invalid_argument(std::string(name) +
                                    " must name a readable regular file");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error(std::string("cannot read ") + name);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

} // namespace

int main()
{
    try
    {
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0)
            throw std::runtime_error("failed to block shutdown signals");

        const char *root = std::getenv("WEB_PHASE11_ONNX_MODEL_ROOT");
        if (!root || !*root)
            throw std::invalid_argument(
                "WEB_PHASE11_ONNX_MODEL_ROOT is required");
        const bool production = environmentFlag("WEB_PRODUCTION_MODE");

        webserver::phase11::InProcessOnnxModelProviderConfig providerConfig;
        providerConfig.modelRoot = root;
        providerConfig.workerCount = environmentSize(
            "WEB_PHASE11_ONNX_WORKERS", 1, 1, 16);
        providerConfig.maxQueued = environmentSize(
            "WEB_PHASE11_ONNX_QUEUE", 8, 1, 1024);
        providerConfig.maxLoadedModels = environmentSize(
            "WEB_PHASE11_ONNX_MODELS", 2, 1, 32);
        providerConfig.maxAdaptersPerModel = environmentSize(
            "WEB_PHASE11_ONNX_ADAPTERS", 8, 1, 128);
        providerConfig.maxPromptBytes = environmentSize(
            "WEB_PHASE11_ONNX_PROMPT_BYTES", 1024 * 1024, 1024,
            16 * 1024 * 1024);
        auto provider = std::make_shared<
            webserver::phase11::InProcessOnnxModelProvider>(providerConfig);

        webserver::phase11::GrpcOnnxWorkerServiceConfig serviceConfig;
        if (const char *token = std::getenv("WEB_PHASE11_ONNX_RPC_TOKEN"))
            serviceConfig.authenticationToken = token;
        if (production && serviceConfig.authenticationToken.size() < 32)
            throw std::invalid_argument(
                "production ONNX worker requires a token with at least 32 bytes");
        serviceConfig.maxBufferedTokens = environmentSize(
            "WEB_PHASE11_ONNX_RPC_BUFFER_TOKENS", 256, 1, 4096);
        serviceConfig.maxBufferedBytes = environmentSize(
            "WEB_PHASE11_ONNX_RPC_BUFFER_BYTES", 256 * 1024, 1024,
            16 * 1024 * 1024);
        webserver::phase11::GrpcOnnxWorkerService service(
            provider, provider, std::move(serviceConfig));

        const char *addressRaw =
            std::getenv("WEB_PHASE11_ONNX_WORKER_ADDRESS");
        const std::string address = addressRaw && *addressRaw
                                        ? addressRaw
                                        : "127.0.0.1:50061";
        grpc::ServerBuilder builder;
        builder.SetMaxReceiveMessageSize(static_cast<int>(environmentSize(
            "WEB_PHASE11_ONNX_RPC_RECEIVE_BYTES", 1024 * 1024, 1024,
            16 * 1024 * 1024)));
        builder.SetMaxSendMessageSize(static_cast<int>(environmentSize(
            "WEB_PHASE11_ONNX_RPC_SEND_BYTES", 1024 * 1024, 1024,
            16 * 1024 * 1024)));

        const char *certificate =
            std::getenv("WEB_PHASE11_ONNX_WORKER_TLS_CERT");
        const char *privateKey =
            std::getenv("WEB_PHASE11_ONNX_WORKER_TLS_KEY");
        if (static_cast<bool>(certificate) != static_cast<bool>(privateKey))
            throw std::invalid_argument(
                "ONNX worker TLS certificate and key must be set together");
        std::shared_ptr<grpc::ServerCredentials> credentials;
        if (certificate)
        {
            grpc::SslServerCredentialsOptions options;
            options.pem_key_cert_pairs.push_back(
                {readRegularFile("WEB_PHASE11_ONNX_WORKER_TLS_KEY", privateKey),
                 readRegularFile("WEB_PHASE11_ONNX_WORKER_TLS_CERT", certificate)});
            credentials = grpc::SslServerCredentials(options);
        }
        else
        {
            if (production)
                throw std::invalid_argument(
                    "production ONNX worker requires TLS certificate and key");
            credentials = grpc::InsecureServerCredentials();
        }

        int boundPort = 0;
        builder.AddListeningPort(address, std::move(credentials), &boundPort);
        builder.RegisterService(&service);
        auto server = builder.BuildAndStart();
        if (!server || boundPort == 0)
            throw std::runtime_error("failed to start ONNX worker gRPC server");
        std::cout << "ONNX worker listening on " << address
                  << " (bound port " << boundPort << ")\n";

        int received = 0;
        if (sigwait(&signals, &received) != 0)
            throw std::runtime_error("sigwait failed");
        std::cout << "received signal " << received
                  << ", draining ONNX worker\n";
        server->Shutdown(std::chrono::system_clock::now() +
                         std::chrono::seconds(5));
        server->Wait();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "ONNX worker failed: " << error.what() << '\n';
        return 1;
    }
}
