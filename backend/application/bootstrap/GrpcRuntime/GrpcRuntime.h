#pragma once
#include <memory>
#include <functional>
#include <string>
namespace webserver::ops {class OperationalMetrics;}
class ServerRuntime;
class Router;
namespace webserver::bootstrap {
class StorageRuntime; class IdentityRuntime; class ChatRuntime; class FileRuntime;
class KnowledgeRuntime; class AiRuntime;
/** Owns one domain's components. Other modules receive explicit borrowed interfaces. */
class GrpcRuntime final {
public:
    GrpcRuntime(bool production,std::string authSecret,std::shared_ptr<ops::OperationalMetrics>);
    void stop()noexcept;
    ~GrpcRuntime();
    GrpcRuntime(const GrpcRuntime&)=delete;
    GrpcRuntime& operator=(const GrpcRuntime&)=delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
