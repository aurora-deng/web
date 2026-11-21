#include "PreferenceInference.h"

#include "client/companion/ProxyServer/ProxyServer.h"
#include <boost/json.hpp>

namespace nebula::client {
namespace {

namespace http = boost::beast::http;
namespace json = boost::json;
constexpr std::string_view local = "/preference-inference";
constexpr std::string_view remote = "/api/ai/private/generations";
constexpr std::string_view digits = "0123456789";
constexpr std::string_view hex = "0123456789abcdef";

bool identifier(std::string_view value) {
    return value.size() == 40 && value.find_first_not_of(hex) == std::string_view::npos;
}

std::string_view field(const json::object& value, std::string_view name) {
    const auto* member = value.if_contains(name);
    if (!member || !member->is_string()) return {};
    const auto& text = member->as_string();
    return {text.data(), text.size()};
}

bool valid(const json::object& payload) {
    if (payload.size() != 8 || !identifier(field(payload, "requestId"))) return false;
    const auto account = field(payload, "aiUserId");
    if (account.empty() || account.size() > 20 || account.find_first_not_of(digits) != std::string_view::npos) return false;
    const auto budget = field(payload, "maxOutputTokens");
    if (budget != "96" && budget != "112" && budget != "320") return false;
    const auto system = field(payload, "content0");
    const auto query = field(payload, "content1");
    return field(payload, "messageCount") == "2" && field(payload, "role0") == "system" &&
        field(payload, "role1") == "user" && !system.empty() && system.size() <= 1800 &&
        !query.empty() && query.size() <= (budget == "320" ? 12000 : 1680);
}

ProxyResponse badRequest() {
    return localJson(400, {{"message", "无效的偏好分析请求"}});
}

}

ProxyResponse handlePreferenceInference(const ProxyRequest& request, std::string_view route, std::string_view query,
    const BoundServerSession& identity, UpstreamTransport& upstream) {
    const auto method = request.method();
    std::string target(remote);
    if (route == local && method == http::verb::post) {
        if (!query.empty() || request.body().size() > 32 * 1024 || !request[http::field::content_type].starts_with("application/json"))
            return badRequest();
        boost::system::error_code error;
        const auto payload = json::parse(request.body(), error);
        if (error || !payload.is_object() || !valid(payload.as_object())) return badRequest();
    } else {
        if (!route.starts_with(local) || route.size() != local.size() + 41 || route[local.size()] != '/') return badRequest();
        const auto id = route.substr(local.size() + 1);
        if (!identifier(id) || !request.body().empty()) return badRequest();
        target.append("/").append(id);
        if (method == http::verb::get && query.starts_with("after=")) {
            const auto after = query.substr(6);
            if (after.empty() || after.size() > 10 || after.find_first_not_of(digits) != std::string_view::npos)
                return badRequest();
            target.append("?").append(query);
        } else if (method != http::verb::delete_ || !query.empty()) {
            return badRequest();
        }
    }
    ProxyRequest forwarded{method, std::move(target), 11};
    if (method == http::verb::post) {
        forwarded.set(http::field::content_type, "application/json");
        forwarded.body() = request.body();
    }
    auto response = upstream.requestBound(identity, std::move(forwarded));
    response.set(http::field::cache_control, "no-store");
    return response;
}

}
