#include "server/phase11/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"

#include "server/Route/Router.h"
#include "server/http/RequestContext/RequestContext.h"
#include "server/phase11/transport/FlatJson/FlatJson.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string_view>

namespace webserver::phase11::transport
{
namespace
{

TimePoint now()
{
    return std::chrono::system_clock::now();
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char current)
                   { return static_cast<char>(std::tolower(current)); });
    return value;
}

std::string statusText(int status)
{
    switch (status)
    {
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 503: return "Service Unavailable";
    default: return status >= 500 ? "Internal Server Error" : "OK";
    }
}

std::pair<int, std::string_view> mapError(ErrorCode code)
{
    switch (code)
    {
    case ErrorCode::InvalidArgument: return {400, "invalid_argument"};
    case ErrorCode::NotFound: return {404, "not_found"};
    case ErrorCode::Conflict: return {409, "conflict"};
    case ErrorCode::Unauthenticated: return {401, "unauthenticated"};
    case ErrorCode::Forbidden: return {403, "forbidden"};
    case ErrorCode::RateLimited: return {429, "rate_limited"};
    case ErrorCode::Unavailable: return {503, "unavailable"};
    }
    return {500, "internal"};
}

void jsonResponse(RequestContext &context, int status, std::string body)
{
    context.response->status = status;
    context.response->statusText = statusText(status);
    context.response->json(body);
    context.response->setHeader("Cache-Control", "no-store");
    context.response->setHeader("Pragma", "no-cache");
}

void errorResponse(RequestContext &context, int status,
                   std::string_view code, std::string_view message)
{
    jsonResponse(context, status,
                 "{\"code\":" + quoteJson(code) +
                     ",\"message\":" + quoteJson(message) + "}");
}

std::string userJson(const User &user)
{
    return "{\"id\":" + std::to_string(user.id) +
           ",\"username\":" + quoteJson(user.username) +
           ",\"displayName\":" + quoteJson(user.displayName) +
           ",\"biography\":" + quoteJson(user.biography) +
           ",\"aiAccount\":" + (user.aiAccount ? "true" : "false") + "}";
}

std::string loginJson(const User &user, std::string_view csrfToken)
{
    return "{\"user\":" + userJson(user) +
           ",\"csrfToken\":" + quoteJson(csrfToken) + "}";
}

std::string requireField(const FlatStringObject &object, std::string_view name)
{
    const auto found = object.find(std::string(name));
    if (found == object.end())
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "missing field: " + std::string(name));
    return found->second;
}

FlatStringObject requestObject(const RequestContext &context,
                               std::size_t maximumBytes)
{
    if (context.request.bodyData.size() > maximumBytes)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "JSON request body is too large");
    const auto type = context.request.headers.find("content-type");
    if (type == context.request.headers.end() ||
        !lower(type->second).starts_with("application/json"))
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "Content-Type must be application/json");
    try
    {
        return parseFlatStringObject(context.request.bodyData);
    }
    catch (const std::invalid_argument &error)
    {
        throw ApplicationError(ErrorCode::InvalidArgument, error.what());
    }
}

std::string cookieValue(std::string_view header, std::string_view wantedName)
{
    std::size_t start = 0;
    while (start < header.size())
    {
        auto end = header.find(';', start);
        if (end == std::string_view::npos)
            end = header.size();
        auto item = header.substr(start, end - start);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front())))
            item.remove_prefix(1);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back())))
            item.remove_suffix(1);
        const auto equals = item.find('=');
        if (equals != std::string_view::npos &&
            item.substr(0, equals) == wantedName)
            return std::string(item.substr(equals + 1));
        start = end + 1;
    }
    return {};
}

std::string sessionToken(const RequestContext &context,
                         std::string_view cookieName)
{
    const auto found = context.request.headers.find("cookie");
    if (found == context.request.headers.end())
        return {};
    return cookieValue(found->second, cookieName);
}

std::string csrfToken(const RequestContext &context)
{
    const auto found = context.request.headers.find("x-csrf-token");
    return found == context.request.headers.end() ? std::string{} : found->second;
}

std::string cookieHeader(const Phase11AuthHttpConfig &config,
                         std::string_view token, bool clear)
{
    std::string value = config.sessionCookieName + "=" + std::string(token) +
                        "; Path=/; HttpOnly; SameSite=Strict";
    if (config.secureCookie)
        value += "; Secure";
    if (clear)
        value += "; Max-Age=0";
    else
        value += "; Max-Age=" +
                 std::to_string(config.sessionLifetime.count());
    return value;
}

std::string loginOrigin(const RequestContext &context,
                        const Phase11AuthHttpConfig &config)
{
    if (!config.trustedClientIpHeader.empty())
    {
        const auto found = context.request.headers.find(
            lower(config.trustedClientIpHeader));
        if (found != context.request.headers.end() && !found->second.empty())
            return found->second;
    }
    return "direct-client";
}

template<class Operation>
bool guarded(RequestContext &context, Operation &&operation)
{
    try
    {
        operation();
    }
    catch (const ApplicationError &error)
    {
        auto [status, code] = mapError(error.code());
        errorResponse(context, status, code, error.what());
    }
    catch (const std::exception &)
    {
        // 内部异常可能带数据库地址或 SQL 细节，只给客户端稳定的通用错误。
        errorResponse(context, 500, "internal", "internal server error");
    }
    return true;
}

} // namespace

Phase11AuthHttp::Phase11AuthHttp(IAuthBusiness &auth,
                                 DatabaseExecutor &databaseExecutor,
                                 Phase11AuthHttpConfig config)
    : auth_(auth), databaseExecutor_(databaseExecutor), config_(std::move(config))
{
    if (config_.sessionCookieName.empty() ||
        config_.sessionCookieName.find_first_of("=; \t\r\n") != std::string::npos)
        throw std::invalid_argument("invalid Phase 11 session cookie name");
    if (config_.maximumJsonBytes == 0 || config_.maximumJsonBytes > 1024 * 1024)
        throw std::invalid_argument("invalid Phase 11 JSON body limit");
    if (config_.sessionLifetime <= std::chrono::seconds::zero())
        throw std::invalid_argument("invalid Phase 11 session lifetime");
}

RestoredSession Phase11AuthHttp::authorizeRequest(
    const RequestContext &context, bool requireCsrf)
{
    auto token = sessionToken(context, config_.sessionCookieName);
    auto csrf = csrfToken(context);
    return runDatabase([this, token = std::move(token),
                        csrf = std::move(csrf), requireCsrf]
    {
        const auto restored = auth_.restoreSession(token, now());
        if (!restored)
            throw ApplicationError(ErrorCode::Unauthenticated,
                                   "authentication required");
        if (requireCsrf && !auth_.verifyCsrf(token, csrf, now()))
            throw ApplicationError(ErrorCode::Forbidden,
                                   "invalid CSRF token");
        return *restored;
    });
}

void Phase11AuthHttp::registerRoutes(Router &router)
{
    router.POST("/api/auth/register", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto fields = requestObject(context, config_.maximumJsonBytes);
            auto username = requireField(fields, "username");
            auto password = requireField(fields, "password");
            auto displayName = fields.contains("displayName")
                                   ? fields.at("displayName") : std::string{};
            const auto user = runDatabase(
                [this, username = std::move(username),
                 password = std::move(password),
                 displayName = std::move(displayName)]() mutable
                {
                    return auth_.registerUser(std::move(username),
                                              std::move(password),
                                              std::move(displayName), now());
                });
            jsonResponse(context, 201, "{\"user\":" + userJson(user) + "}");
        });
    });

    router.POST("/api/auth/login", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto fields = requestObject(context, config_.maximumJsonBytes);
            auto username = requireField(fields, "username");
            auto password = requireField(fields, "password");
            auto origin = loginOrigin(context, config_);
            const auto login = runDatabase(
                [this, username = std::move(username),
                 password = std::move(password), origin = std::move(origin)]
                {
                    return auth_.login(username, password, now(), origin);
                });
            jsonResponse(context, 200, loginJson(login.user, login.csrfToken));
            context.response->setHeader(
                "Set-Cookie", cookieHeader(config_, login.sessionToken, false));
        });
    });

    router.GET("/api/me", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto restored = authorizeRequest(context);
            jsonResponse(context, 200,
                         loginJson(restored.user, restored.csrfToken));
        });
    });

    router.PATCH("/api/me", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto fields = requestObject(context, config_.maximumJsonBytes);
            const auto identity = authorizeRequest(context, true);
            const auto user = runDatabase([this, identity, fields]
            {
                const auto displayName = fields.contains("displayName")
                                             ? fields.at("displayName")
                                             : identity.user.displayName;
                const auto biography = fields.contains("biography")
                                           ? fields.at("biography")
                                           : identity.user.biography;
                return auth_.updateProfile(identity.user.id, displayName,
                                           biography);
            });
            jsonResponse(context, 200, "{\"user\":" + userJson(user) + "}");
        });
    });

    router.POST("/api/auth/logout", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            (void)authorizeRequest(context, true);
            auto token = sessionToken(context, config_.sessionCookieName);
            runDatabase([this, token]
            {
                auth_.logout(token, now());
            });
            jsonResponse(context, 200, "{\"ok\":true}");
            context.response->setHeader(
                "Set-Cookie", cookieHeader(config_, "", true));
        });
    });
}

} // namespace webserver::phase11::transport
