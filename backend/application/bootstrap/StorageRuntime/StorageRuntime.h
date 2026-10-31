#pragma once
#include <memory>
#include <functional>
#include <string>
#include "backend/application/runtime/BoundedExecutor/BoundedExecutor.h"
namespace webserver::phase11 {class PostgresStore;}
class ServerRuntime;
class Router;
namespace webserver::bootstrap {
class StorageRuntime; class IdentityRuntime; class ChatRuntime; class FileRuntime;
class KnowledgeRuntime; class AiRuntime;
/** Owns one domain's components. Other modules receive explicit borrowed interfaces. */
class StorageRuntime final {
public:
    StorageRuntime(std::string connection,bool deliveryPool);
    phase11::PostgresStore& store();
    phase11::PostgresStore& delivery();
    phase11::PostgresStore* deliveryMetrics() noexcept;
    phase11::BoundedExecutor& executor();
    const std::string& connection()const;
    void drain() noexcept;
    ~StorageRuntime();
    StorageRuntime(const StorageRuntime&)=delete;
    StorageRuntime& operator=(const StorageRuntime&)=delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
