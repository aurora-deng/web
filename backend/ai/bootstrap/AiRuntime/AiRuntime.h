#pragma once
#include <memory>
#include <functional>
#include <string>
#include <vector>
namespace webserver::phase11 {class ModelRegistryService;class ModelEvaluationService;class AiChatService;class ResilientModelProvider;}
class ServerRuntime;
class Router;
namespace webserver::bootstrap {
class StorageRuntime; class IdentityRuntime; class ChatRuntime; class FileRuntime;
class KnowledgeRuntime; class AiRuntime;
/** Owns one domain's components. Other modules receive explicit borrowed interfaces. */
class AiRuntime final {
public:
    AiRuntime(StorageRuntime&,IdentityRuntime&,ChatRuntime&,KnowledgeRuntime&,ServerRuntime&,bool production);
    phase11::ModelRegistryService& registry();
    phase11::ModelEvaluationService& evaluations();
    phase11::AiChatService& chat();
    const std::vector<std::shared_ptr<phase11::ResilientModelProvider>>& resilient()const;
    void registerRoutes(Router&);
    void cancelPrivate()noexcept;
    void stop()noexcept;
    ~AiRuntime();
    AiRuntime(const AiRuntime&)=delete;
    AiRuntime& operator=(const AiRuntime&)=delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
