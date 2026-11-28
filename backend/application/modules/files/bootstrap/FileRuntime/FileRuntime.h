#pragma once
#include <memory>
#include <functional>
#include <string>
#include "backend/application/runtime/BoundedExecutor/BoundedExecutor.h"
namespace webserver::phase11 {class FileService;}
class ServerRuntime;
class Router;
namespace webserver::bootstrap {
class StorageRuntime; class IdentityRuntime; class ChatRuntime; class FileRuntime;
class KnowledgeRuntime; class AiRuntime;
/** Owns one domain's components. Other modules receive explicit borrowed interfaces. */
class FileRuntime final {
public:
    explicit FileRuntime(StorageRuntime&);
    phase11::FileService& service();
    phase11::BoundedExecutor& executor();
    void bind(IdentityRuntime&,ChatRuntime&,std::function<void()> committed);
    void registerRoutes(Router&);
    void drain()noexcept;
    ~FileRuntime();
    FileRuntime(const FileRuntime&)=delete;
    FileRuntime& operator=(const FileRuntime&)=delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
