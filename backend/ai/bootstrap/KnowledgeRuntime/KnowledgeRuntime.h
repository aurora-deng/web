#pragma once
#include <memory>
#include <functional>
#include <string>
#include "backend/application/runtime/BoundedExecutor/BoundedExecutor.h"
namespace webserver::phase11 {class RagService;class DeterministicKnowledgeIndex;}
class ServerRuntime;
class Router;
namespace webserver::bootstrap {
class StorageRuntime; class IdentityRuntime; class ChatRuntime; class FileRuntime;
class KnowledgeRuntime; class AiRuntime;
/** Owns one domain's components. Other modules receive explicit borrowed interfaces. */
class KnowledgeRuntime final {
public:
    KnowledgeRuntime(StorageRuntime&,IdentityRuntime&);
    phase11::RagService& rag();
    phase11::DeterministicKnowledgeIndex& index();
    phase11::BoundedExecutor& executor();
    std::size_t maxChunks()const;
    void registerRoutes(Router&);
    void drain()noexcept;
    ~KnowledgeRuntime();
    KnowledgeRuntime(const KnowledgeRuntime&)=delete;
    KnowledgeRuntime& operator=(const KnowledgeRuntime&)=delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
