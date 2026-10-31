#include "backend/application/bootstrap/GrpcRuntime/GrpcRuntime.h"
#include "backend/application/bootstrap/BootstrapSettings/BootstrapSettings.h"

#ifdef WEBSERVER_HAS_GRPC
#include "backend/server/grpc/GrpcServer.h"
#endif
#include <iostream>
namespace webserver::bootstrap {
struct GrpcRuntime::Impl {
#ifdef WEBSERVER_HAS_GRPC
    std::unique_ptr<grpc_runtime::GrpcServer> server;
#endif
};
GrpcRuntime::GrpcRuntime(bool productionMode,std::string authSecret,std::shared_ptr<ops::OperationalMetrics>operationalMetrics):impl_(std::make_unique<Impl>()){
#ifdef WEBSERVER_HAS_GRPC
        const char *configuredGrpcAddress = std::getenv("WEB_GRPC_ADDRESS");
        const std::string grpcAddress = configuredGrpcAddress
                                            ? configuredGrpcAddress
                                            : "127.0.0.1:50051";
        if (grpcAddress != "off")
        {
            webserver::grpc_runtime::GrpcServerOptions grpcOptions;
            grpcOptions.address = grpcAddress;
            grpcOptions.authenticationSecret = authSecret;
            grpcOptions.metrics = operationalMetrics;
            grpcOptions.maxConcurrentRpcs = size(
                "WEB_GRPC_MAX_CONCURRENT_RPCS", 256, 1, 100'000);
            grpcOptions.maxWorkerThreads = size(
                "WEB_GRPC_MAX_WORKER_THREADS", 64, 1, 4096);
            grpcOptions.maxReceiveMessageBytes = static_cast<int>(
                size(
                    "WEB_GRPC_MAX_RECEIVE_BYTES", 1024 * 1024, 1024,
                    64 * 1024 * 1024));
            grpcOptions.maxSendMessageBytes = static_cast<int>(
                size(
                    "WEB_GRPC_MAX_SEND_BYTES", 1024 * 1024, 1024,
                    64 * 1024 * 1024));
            grpcOptions.maxRpcDuration = std::chrono::milliseconds(
                size(
                    "WEB_GRPC_MAX_RPC_MS", 30'000, 1,
                    24 * 60 * 60 * 1000));

            const char *grpcCert = std::getenv("WEB_GRPC_TLS_CERT");
            const char *grpcKey = std::getenv("WEB_GRPC_TLS_KEY");
            if (static_cast<bool>(grpcCert) != static_cast<bool>(grpcKey))
                throw std::invalid_argument(
                    "WEB_GRPC_TLS_CERT and WEB_GRPC_TLS_KEY must be set together");
            if (grpcCert)
            {
                grpcOptions.certificateChainPath = grpcCert;
                grpcOptions.privateKeyPath = grpcKey;
            }
            else if (productionMode)
            {
                throw std::invalid_argument(
                    "WEB_PRODUCTION_MODE requires WEB_GRPC_TLS_CERT and WEB_GRPC_TLS_KEY");
            }

            impl_->server = std::make_unique<webserver::grpc_runtime::GrpcServer>(
                std::move(grpcOptions));
            impl_->server->start();
            std::cout << "gRPC listening on " << grpcAddress
                      << " (bound port " << impl_->server->boundPort() << ")\n";
        }
#else
    (void)productionMode;(void)authSecret;(void)operationalMetrics;
#endif
}
GrpcRuntime::~GrpcRuntime(){stop();}
void GrpcRuntime::stop()noexcept{
#ifdef WEBSERVER_HAS_GRPC
    if(impl_->server)impl_->server->stop();
#endif
}
}
