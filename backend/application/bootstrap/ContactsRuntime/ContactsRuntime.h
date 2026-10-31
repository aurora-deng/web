#pragma once
#include <memory>
#include <functional>
class Router;class ServerRuntime;
namespace webserver::phase11 {class SocialService;}
namespace webserver::bootstrap {
class StorageRuntime;class IdentityRuntime;
/** Contacts own relationship rules and HTTP entry; chat borrows only its service. */
class ContactsRuntime final {
public:
    explicit ContactsRuntime(StorageRuntime&);
    ~ContactsRuntime();
    phase11::SocialService& service();
    void bind(IdentityRuntime&,ServerRuntime&,std::function<void()> committed);
    void registerRoutes(Router&);
private:struct Impl;std::unique_ptr<Impl>impl_;
};
}
