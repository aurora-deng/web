#pragma once
#include <memory>
#include <functional>
#include <string>
namespace webserver::phase11 {class SocialService;class ChatService;}
class ServerRuntime;
class Router;
namespace webserver::bootstrap {
class StorageRuntime; class IdentityRuntime; class ChatRuntime; class FileRuntime;
class KnowledgeRuntime; class AiRuntime;
/** Owns one domain's components. Other modules receive explicit borrowed interfaces. */
class ChatRuntime final {
public:
    explicit ChatRuntime(StorageRuntime&);
    phase11::ChatService& chat();
    void bind(IdentityRuntime&,ServerRuntime&,std::function<void()> committed);
    void registerRoutes(Router&);
    ~ChatRuntime();
    ChatRuntime(const ChatRuntime&)=delete;
    ChatRuntime& operator=(const ChatRuntime&)=delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
