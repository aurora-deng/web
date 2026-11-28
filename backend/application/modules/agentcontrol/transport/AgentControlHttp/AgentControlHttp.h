#pragma once
#include "backend/application/modules/shared/infrastructure/Phase11Bridge/Phase11Bridge.h"
#include "backend/application/modules/accounts/transport/Phase11AuthHttp/Phase11AuthHttp.h"
#include <cstdint>
#include <vector>
class Router;
struct RequestContext;
namespace webserver::function3::agentcontrol {
// Native authentication authority. Executable recipes live in the existing
// account workspace component, not in a second HTTP listener or registry.
class AgentControlHttp final {
public:
    AgentControlHttp(phase11::transport::Phase11AuthHttp& auth,
                     phase11::BoundedExecutor& executor, IActorDirectory& actors);
    void registerRoutes(Router& router);
private:
    static std::vector<std::uint64_t> configuredIds(const char* name);
    static bool contains(const std::vector<std::uint64_t>& ids, std::uint64_t id);
    static bool enabled();
    Actor requireAccess(RequestContext& context) const;
    phase11::transport::Phase11AuthHttp& auth_;
    phase11::BoundedExecutor& executor_;
    IActorDirectory& actors_;
    const std::vector<std::uint64_t> operators_;
    const std::vector<std::uint64_t> approvers_;
};
} // namespace webserver::function3::agentcontrol
