#include "server/phase11/transport/http/Phase11TrainingHttp/Phase11TrainingHttp.h"

#include "server/Route/Router.h"
#include "server/http/RequestContext/RequestContext.h"
#include "server/phase11/transport/FlatJson/FlatJson.h"

#include <charconv>
#include <chrono>
#include <stdexcept>
#include <string_view>

namespace webserver::phase11::transport
{
namespace
{

TimePoint now() { return std::chrono::system_clock::now(); }

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

void reply(RequestContext &context, int status, std::string body)
{
    context.response->status = status;
    context.response->statusText = statusText(status);
    context.response->json(body);
    context.response->setHeader("Cache-Control", "no-store");
}

template<class Operation>
bool guarded(RequestContext &context, Operation &&operation)
{
    try { operation(); }
    catch (const ApplicationError &failure)
    {
        const auto [status, code] = mapError(failure.code());
        reply(context, status,
              "{\"code\":" + quoteJson(code) +
              ",\"message\":" + quoteJson(failure.what()) + "}");
    }
    catch (const std::exception &)
    {
        reply(context, 500,
              "{\"code\":\"internal\",\"message\":\"internal server error\"}");
    }
    return true;
}

FlatStringObject jsonBody(const RequestContext &context,
                          std::size_t maximumBytes)
{
    if (context.request.bodyData.size() > maximumBytes)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "training request body is too large");
    const auto type = context.request.headers.find("content-type");
    if (type == context.request.headers.end() ||
        !std::string_view(type->second).starts_with("application/json"))
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "Content-Type must be application/json");
    try { return parseFlatStringObject(context.request.bodyData); }
    catch (const std::invalid_argument &failure)
    {
        throw ApplicationError(ErrorCode::InvalidArgument, failure.what());
    }
}

std::string required(const FlatStringObject &fields, std::string_view name)
{
    const auto found = fields.find(std::string(name));
    if (found == fields.end())
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "missing field: " + std::string(name));
    return found->second;
}

std::uint64_t positiveInteger(std::string_view value, std::string_view name)
{
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(),
                                        result);
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size() || result == 0)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid positive integer: " + std::string(name));
    return result;
}

std::string stateName(TrainingCandidateState state)
{
    switch (state)
    {
    case TrainingCandidateState::Submitted: return "submitted";
    case TrainingCandidateState::Approved: return "approved";
    case TrainingCandidateState::Rejected: return "rejected";
    case TrainingCandidateState::Exported: return "exported";
    case TrainingCandidateState::Revoked: return "revoked";
    }
    return "unknown";
}

std::string candidateJson(const TrainingCandidate &candidate)
{
    return "{\"id\":" + std::to_string(candidate.id) +
           ",\"sourceMessageId\":" +
           std::to_string(candidate.sourceMessageId) +
           ",\"sanitizedPrompt\":" +
           quoteJson(candidate.sanitizedPrompt) +
           ",\"sanitizedResponse\":" +
           quoteJson(candidate.sanitizedResponse) +
           ",\"consentVersion\":" + quoteJson(candidate.consentVersion) +
           ",\"state\":" + quoteJson(stateName(candidate.state)) +
           ",\"datasetVersionId\":" +
           (candidate.datasetVersionId
                ? std::to_string(*candidate.datasetVersionId) : "null") + "}";
}

std::string candidatesJson(const std::vector<TrainingCandidate> &candidates)
{
    std::string result = "[";
    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        if (index) result.push_back(',');
        result += candidateJson(candidates[index]);
    }
    result.push_back(']');
    return result;
}

} // namespace

void Phase11TrainingHttp::registerRoutes(Router &router)
{
    router.POST("/api/training/candidates", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto fields = jsonBody(context, config_.maximumRequestBytes);
            if (required(fields, "consentGranted") != "true")
                throw ApplicationError(ErrorCode::Forbidden,
                                       "explicit training consent is required");
            const auto candidate = runDatabase(
                [this, owner = identity.user.id,
                 message = positiveInteger(required(fields, "sourceMessageId"),
                                           "sourceMessageId"),
                 prompt = required(fields, "sanitizedPrompt"),
                 response = required(fields, "sanitizedResponse")]
                {
                    return training_.submitTrainingCandidate(
                        {owner, message, prompt, response,
                         config_.consentVersion, true}, now());
                });
            reply(context, 201, candidateJson(candidate));
        });
    });

    router.GET("/api/training/candidates", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto candidates = runDatabase(
                [this, owner = identity.user.id]
                { return training_.trainingCandidates(owner); });
            reply(context, 200, candidatesJson(candidates));
        });
    });

    router.POST("/api/training/candidates/:candidate/revoke",
                [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto found = context.params.find("candidate");
            if (found == context.params.end())
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "candidate id is required");
            const auto candidate = runDatabase(
                [this, owner = identity.user.id,
                 id = positiveInteger(found->second, "candidate")]
                { return training_.revokeTrainingCandidate(id, owner); });
            reply(context, 200, candidateJson(candidate));
        });
    });
}

} // namespace webserver::phase11::transport
