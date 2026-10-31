#pragma once
#include <memory>
#include <functional>
#include <string>
namespace webserver::ops {class OperationalMetrics;}
namespace webserver::phase11::transport {class Phase11AuthHttp;}
class ServerRuntime;
class Router;
namespace webserver::bootstrap {
class StorageRuntime; class IdentityRuntime; class ChatRuntime; class FileRuntime;
class KnowledgeRuntime; class AiRuntime;
/** Owns one domain's components. Other modules receive explicit borrowed interfaces. */
class OperationalRuntime final {
public:
    explicit OperationalRuntime(bool production);
    std::shared_ptr<ops::OperationalMetrics> metrics()const;
    const std::string& authSecret()const;
    void install(ServerRuntime&,phase11::transport::Phase11AuthHttp*,std::function<bool()> ready,std::function<std::string()> metrics,bool schemaVerified);
    ~OperationalRuntime();
    OperationalRuntime(const OperationalRuntime&)=delete;
    OperationalRuntime& operator=(const OperationalRuntime&)=delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
