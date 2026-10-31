#pragma once
#include <memory>
#include <functional>
#include <string>
namespace webserver::phase11::transport {class Phase11AuthHttp;}
class ServerRuntime;
class Router;
namespace webserver::bootstrap {
class StorageRuntime; class IdentityRuntime; class ChatRuntime; class FileRuntime;
class KnowledgeRuntime; class AiRuntime;
/** Owns one domain's components. Other modules receive explicit borrowed interfaces. */
class BusinessRuntime final {
public:
    BusinessRuntime(ServerRuntime&,bool production);
    bool enabled()const noexcept;
    phase11::transport::Phase11AuthHttp* authentication()const noexcept;
    bool ready()const;
    std::string metrics()const;
    void registerRoutes(Router&);
    void stop()noexcept;
    bool stoppedCleanly()const noexcept;
    ~BusinessRuntime();
    BusinessRuntime(const BusinessRuntime&)=delete;
    BusinessRuntime& operator=(const BusinessRuntime&)=delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
