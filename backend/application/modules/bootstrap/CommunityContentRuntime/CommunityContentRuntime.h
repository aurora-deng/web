#pragma once

#include <memory>

class Router;
namespace webserver::phase11 {
class BoundedExecutor;
class IFileBusiness;
class ISocialBusiness;
namespace transport { class Phase11AuthHttp; }
}
namespace webserver::function3 {
class IEventWriter;
class IQueryCache;
class ModerationService;
class Phase11Bridge;
class SqlContext;
class TreeService;

/** Owns the public story, activity, moderation and media adapters as one content module. */
class CommunityContentRuntime final {
public:
    CommunityContentRuntime(SqlContext&, Phase11Bridge&, IEventWriter&,
                            phase11::transport::Phase11AuthHttp&, phase11::BoundedExecutor&,
                            phase11::IFileBusiness&, phase11::ISocialBusiness&);
    ~CommunityContentRuntime();
    CommunityContentRuntime(const CommunityContentRuntime&) = delete;
    CommunityContentRuntime& operator=(const CommunityContentRuntime&) = delete;

    TreeService& tree() noexcept;
    ModerationService& moderation() noexcept;
    void setCache(IQueryCache*) noexcept;
    void registerRoutes(Router&);
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
