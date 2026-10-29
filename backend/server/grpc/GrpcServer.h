#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

namespace webserver::ops
{
class OperationalMetrics;
}

namespace webserver::grpc_runtime
{

/**
 * @brief gRPC 独立监听器的启动参数。
 *
 * gRPC 和现有 Web 服务器处于同一进程，但拥有独立端口。这样官方 gRPC
 * 可以完整管理自己的 HTTP/2 stream、HPACK、trailers 与流控，而不会和
 * 项目原有 Http2Session 的状态机互相抢管同一个连接。
 */
struct GrpcServerOptions
{
    // 默认只监听回环地址，避免开发机一启动就把未配置 TLS 的 RPC 暴露到局域网。
    std::string address{"127.0.0.1:50051"};
    std::string certificateChainPath;
    std::string privateKeyPath;
    std::string authenticationSecret;
    int maxReceiveMessageBytes{1024 * 1024};
    int maxSendMessageBytes{1024 * 1024};
    // 业务准入上限与 Core 线程预算分开：Callback 流等待时不长期占住线程，
    // 但每个在途 RPC 仍持有 Reactor、消息缓冲和协议状态。
    std::size_t maxConcurrentRpcs{256};
    std::size_t maxWorkerThreads{64};
    std::chrono::milliseconds maxRpcDuration{std::chrono::seconds(30)};
    std::shared_ptr<webserver::ops::OperationalMetrics> metrics;
};

/**
 * @brief 官方 gRPC C++ Server 的生命周期外壳。
 *
 * 头文件使用 PImpl 隐藏 gRPC/Protobuf 生成类型，避免把第三方头文件扩散到
 * 现有 Reactor、Router 和协议 Session。析构时会发起有期限的优雅停止。
 */
class GrpcServer
{
public:
    explicit GrpcServer(GrpcServerOptions options = {});
    ~GrpcServer();

    GrpcServer(const GrpcServer &) = delete;
    GrpcServer &operator=(const GrpcServer &) = delete;
    GrpcServer(GrpcServer &&) = delete;
    GrpcServer &operator=(GrpcServer &&) = delete;

    void start();
    void stop(std::chrono::milliseconds grace = std::chrono::seconds(5)) noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] int boundPort() const noexcept;
    [[nodiscard]] const std::string &configuredAddress() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace webserver::grpc_runtime
