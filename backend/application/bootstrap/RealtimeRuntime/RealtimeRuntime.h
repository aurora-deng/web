#pragma once
#include <memory>
#include <functional>
#include <string>
namespace webserver::function3 {class Function3Runtime;}
namespace webserver::phase11::transport {class Phase11OutboxPump;}
class ServerRuntime;
class Router;
namespace webserver::bootstrap {
class StorageRuntime; class IdentityRuntime; class ChatRuntime; class FileRuntime;
class KnowledgeRuntime; class AiRuntime; class ContactsRuntime;
/** Owns one domain's components. Other modules receive explicit borrowed interfaces. */
class RealtimeRuntime final {
public:
    RealtimeRuntime(StorageRuntime&,ContactsRuntime&,ChatRuntime&,AiRuntime&,ServerRuntime&,function3::Function3Runtime*);
    std::function<void()> notifier();
    phase11::transport::Phase11OutboxPump& pump();
    void detachPresence()noexcept;
    void stop()noexcept;
    ~RealtimeRuntime();
    RealtimeRuntime(const RealtimeRuntime&)=delete;
    RealtimeRuntime& operator=(const RealtimeRuntime&)=delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
