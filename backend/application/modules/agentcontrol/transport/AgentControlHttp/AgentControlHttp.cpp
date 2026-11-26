#include "AgentControlHttp.h"
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
#include "backend/server/http/RequestContext/RequestContext.h"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string_view>
namespace webserver::function3::agentcontrol {
namespace http = webserver::function3::http;
AgentControlHttp::AgentControlHttp(phase11::transport::Phase11AuthHttp& auth,
                                  phase11::BoundedExecutor& executor,
                                  IActorDirectory& actors)
    : auth_(auth), executor_(executor), actors_(actors),
      operators_(configuredIds("WEB_AGENT_CONTROL_USER_IDS")),
      approvers_(configuredIds("WEB_AGENT_CONTROL_APPROVER_IDS")) {}
bool AgentControlHttp::enabled() {
    const auto* value = std::getenv("WEB_AGENT_CONTROL_ENABLE");
    return value && std::string_view(value) == "1";
}
std::vector<std::uint64_t> AgentControlHttp::configuredIds(const char* name) {
    const char* raw = std::getenv(name);
    std::string_view value = raw ? raw : "";
    std::vector<std::uint64_t> ids;
    while (!value.empty()) {
        const auto comma = value.find(',');
        const auto token = value.substr(0, comma);
        std::uint64_t id = 0;
        const auto parsed = std::from_chars(token.data(), token.data()+token.size(), id);
        if (token.empty() || parsed.ec != std::errc{} || parsed.ptr != token.data()+token.size() || id == 0)
            throw std::invalid_argument(std::string(name)+" contains an invalid user id");
        ids.push_back(id);
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma+1);
        if (value.empty()) throw std::invalid_argument(std::string(name)+" has a trailing separator");
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}
bool AgentControlHttp::contains(const std::vector<std::uint64_t>& ids, std::uint64_t id) {
    return std::binary_search(ids.begin(), ids.end(), id);
}
Actor AgentControlHttp::requireAccess(RequestContext& context) const {
    const auto session = auth_.authorizeRequest(context, false);
    const auto actor = actors_.requireActor(session.user.id);
    // No administrator fallback, hidden account, or service-actor access.
    if (!enabled() || !actor.interactive || (!contains(operators_, actor.id) && !contains(approvers_, actor.id)))
        throw BusinessError(Failure::NotFound, "control center not found");
    return actor;
}
void AgentControlHttp::registerRoutes(Router& router) {
    if (!enabled()) return;
    auto scope = http::executor(router, executor_);
    auto asset = [this](RequestContext& context, const std::string& name) {
        return http::guarded(context, [&] {
            (void)requireAccess(context);
            const auto path = (std::filesystem::path("./frontend/phase11/agent-control")/name).string();
            context.response->setHeader("Content-Type", name == "index.html" ? "text/html; charset=utf-8" :
                name == "app.js" ? "text/javascript; charset=utf-8" : "text/css; charset=utf-8");
            context.response->setHeader("Cache-Control", "no-store");
            context.response->setHeader("X-Content-Type-Options", "nosniff");
            context.response->setHeader("Content-Security-Policy", "default-src 'self'; object-src 'none'; base-uri 'self'; frame-ancestors 'none'");
            if (!context.response->sendfile(path, context.request, context.request.range)) {
                context.response->status = 404;
                context.response->statusText = "Not Found";
                context.response->text("Not Found");
            }
        });
    };
    router.GET("/phase11/agent-control", [asset](RequestContext& c) { return asset(c,"index.html"); });
    router.GET("/phase11/agent-control/", [asset](RequestContext& c) { return asset(c,"index.html"); });
    router.GET("/phase11/agent-control/app.js", [asset](RequestContext& c) { return asset(c,"app.js"); });
    router.GET("/phase11/agent-control/style.css", [asset](RequestContext& c) { return asset(c,"style.css"); });
    router.GET("/api/private/agent-control/access", [this](RequestContext& context) {
        return http::guarded(context, [&] {
            const auto actor = requireAccess(context);
            const auto session = auth_.authorizeRequest(context, false);
            http::reply(context, "{\"id\":"+http::id(actor.id)+
                ",\"approver\":"+(contains(approvers_,actor.id) ? "true" : "false")+
                ",\"administrator\":"+(actor.role == Role::Administrator ? "true" : "false")+
                ",\"csrfToken\":"+http::quote(session.csrfToken)+"}");
        });
    });
}
} // namespace webserver::function3::agentcontrol
