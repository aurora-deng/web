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
class IdentityRuntime final {
public:
    IdentityRuntime(StorageRuntime&,bool production);
    phase11::transport::Phase11AuthHttp& http();
    ~IdentityRuntime();
    IdentityRuntime(const IdentityRuntime&)=delete;
    IdentityRuntime& operator=(const IdentityRuntime&)=delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
