#include "server/grpc/GrpcServer.h"
#include "server/ops/OperationalMetrics.h"

#include <pthread.h>
#include <signal.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
std::size_t environmentSize(const char *name,
                            std::size_t fallback,
                            std::size_t minimum,
                            std::size_t maximum)
{
    const char *raw = std::getenv(name);
    if (!raw)
        return fallback;
    char *end = nullptr;
    errno = 0;
    const auto value = std::strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' ||
        value < minimum || value > maximum)
        throw std::invalid_argument(std::string(name) + " is outside the allowed range");
    return static_cast<std::size_t>(value);
}

bool environmentFlag(const char *name)
{
    const char *raw = std::getenv(name);
    return raw && (std::string(raw) == "1" || std::string(raw) == "true" ||
                   std::string(raw) == "TRUE");
}
} // namespace

int main()
{
    try
    {
        // 在 gRPC 创建内部线程前阻塞停止信号；主线程随后用 sigwait 同步收信号，
        // 避免在异步信号处理函数里调用并非 signal-safe 的 gRPC Shutdown。
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0)
            throw std::runtime_error("failed to block shutdown signals");

        const bool production = environmentFlag("WEB_PRODUCTION_MODE");
        webserver::grpc_runtime::GrpcServerOptions options;
        if (const char *address = std::getenv("WEB_GRPC_ADDRESS"))
            options.address = address;
        if (options.address == "off")
            throw std::invalid_argument("standalone gRPC server cannot use WEB_GRPC_ADDRESS=off");

        if (const char *secret = std::getenv("WEB_AUTH_SECRET"))
            options.authenticationSecret = secret;
        if (production && options.authenticationSecret.size() < 32)
            throw std::invalid_argument(
                "WEB_PRODUCTION_MODE requires WEB_AUTH_SECRET with at least 32 bytes");

        options.maxConcurrentRpcs = environmentSize(
            "WEB_GRPC_MAX_CONCURRENT_RPCS", 256, 1, 100'000);
        options.maxWorkerThreads = environmentSize(
            "WEB_GRPC_MAX_WORKER_THREADS", 64, 1, 4096);
        options.maxReceiveMessageBytes = static_cast<int>(environmentSize(
            "WEB_GRPC_MAX_RECEIVE_BYTES", 1024 * 1024, 1024, 64 * 1024 * 1024));
        options.maxSendMessageBytes = static_cast<int>(environmentSize(
            "WEB_GRPC_MAX_SEND_BYTES", 1024 * 1024, 1024, 64 * 1024 * 1024));
        options.maxRpcDuration = std::chrono::milliseconds(environmentSize(
            "WEB_GRPC_MAX_RPC_MS", 30'000, 1, 24 * 60 * 60 * 1000));
        options.metrics = std::make_shared<webserver::ops::OperationalMetrics>();

        const char *certificate = std::getenv("WEB_GRPC_TLS_CERT");
        const char *privateKey = std::getenv("WEB_GRPC_TLS_KEY");
        if (static_cast<bool>(certificate) != static_cast<bool>(privateKey))
            throw std::invalid_argument(
                "WEB_GRPC_TLS_CERT and WEB_GRPC_TLS_KEY must be set together");
        if (certificate)
        {
            options.certificateChainPath = certificate;
            options.privateKeyPath = privateKey;
        }
        else if (production)
        {
            throw std::invalid_argument(
                "WEB_PRODUCTION_MODE requires WEB_GRPC_TLS_CERT and WEB_GRPC_TLS_KEY");
        }

        webserver::grpc_runtime::GrpcServer server(std::move(options));
        server.start();
        std::cout << "standalone gRPC listening on " << server.configuredAddress()
                  << " (bound port " << server.boundPort() << ")\n";

        int received = 0;
        if (sigwait(&signals, &received) != 0)
            throw std::runtime_error("sigwait failed");
        std::cout << "received signal " << received << ", draining gRPC\n";
        server.stop(std::chrono::seconds(5));
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "standalone gRPC failed: " << error.what() << '\n';
        return 1;
    }
}
