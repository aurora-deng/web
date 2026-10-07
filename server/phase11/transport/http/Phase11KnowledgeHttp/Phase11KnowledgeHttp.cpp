#include "server/phase11/transport/http/Phase11KnowledgeHttp/Phase11KnowledgeHttp.h"

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
    case 413: return "Payload Too Large";
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
    try
    {
        operation();
    }
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
                               "knowledge request body is too large");
    const auto type = context.request.headers.find("content-type");
    if (type == context.request.headers.end() ||
        !std::string_view(type->second).starts_with("application/json"))
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "Content-Type must be application/json");
    try
    {
        return parseFlatStringObject(context.request.bodyData);
    }
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

std::optional<ConversationId> conversationField(
    const FlatStringObject &fields)
{
    const auto found = fields.find("conversationId");
    if (found == fields.end() || found->second.empty()) return std::nullopt;
    return positiveInteger(found->second, "conversationId");
}

std::optional<ConversationId> conversationQuery(
    const RequestContext &context)
{
    const auto found = context.request.querryParams.find("conversationId");
    if (found == context.request.querryParams.end() || found->second.empty())
        return std::nullopt;
    return positiveInteger(found->second, "conversationId");
}

std::string scopeName(KnowledgeScope scope)
{
    return scope == KnowledgeScope::PrivateUser ? "private" : "conversation";
}

std::string stateName(KnowledgeDocumentState state)
{
    switch (state)
    {
    case KnowledgeDocumentState::Indexing: return "indexing";
    case KnowledgeDocumentState::Ready: return "ready";
    case KnowledgeDocumentState::Failed: return "failed";
    case KnowledgeDocumentState::Deleted: return "deleted";
    }
    return "unknown";
}

std::string documentJson(const KnowledgeDocument &document)
{
    return "{\"id\":" + std::to_string(document.id) +
           ",\"ownerId\":" + std::to_string(document.ownerId) +
           ",\"scope\":" + quoteJson(scopeName(document.scope)) +
           ",\"conversationId\":" +
           (document.conversationId
                ? std::to_string(*document.conversationId) : "null") +
           ",\"title\":" + quoteJson(document.title) +
           ",\"sourceName\":" + quoteJson(document.sourceName) +
           ",\"mediaType\":" + quoteJson(document.mediaType) +
           ",\"sha256\":" + quoteJson(document.sha256) +
           ",\"state\":" + quoteJson(stateName(document.state)) + "}";
}

std::string documentsJson(const std::vector<KnowledgeDocument> &documents)
{
    std::string result = "[";
    for (std::size_t index = 0; index < documents.size(); ++index)
    {
        if (index) result.push_back(',');
        result += documentJson(documents[index]);
    }
    result.push_back(']');
    return result;
}

std::string citationsJson(const std::vector<KnowledgeCitationView> &citations)
{
    std::string result = "[";
    for (std::size_t index = 0; index < citations.size(); ++index)
    {
        if (index) result.push_back(',');
        const auto &view = citations[index];
        result += "{\"rank\":" + std::to_string(view.citation.rank) +
                  ",\"documentId\":" +
                  std::to_string(view.citation.documentId) +
                  ",\"chunkId\":" +
                  std::to_string(view.citation.chunkId) +
                  ",\"score\":" + std::to_string(view.citation.score) +
                  ",\"documentTitle\":" + quoteJson(view.documentTitle) +
                  ",\"sourceName\":" + quoteJson(view.sourceName) +
                  ",\"chunkOrdinal\":" +
                  std::to_string(view.chunkOrdinal) +
                  ",\"content\":" + quoteJson(view.content) + "}";
    }
    result.push_back(']');
    return result;
}

} // namespace

void Phase11KnowledgeHttp::registerRoutes(Router &router)
{
    router.POST("/api/knowledge/documents", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto fields = jsonBody(context, config_.maximumRequestBytes);
            const auto document = runKnowledge(
                [this, owner = identity.user.id,
                 conversation = conversationField(fields),
                 title = required(fields, "title"),
                 source = required(fields, "sourceName"),
                 mediaType = required(fields, "mediaType"),
                 content = required(fields, "content")]
                {
                    return knowledge_.registerKnowledgeDocument(
                        owner, conversation, title, source, mediaType,
                        content, now());
                });
            reply(context, 201, documentJson(document));
        });
    });

    router.GET("/api/knowledge/documents", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto conversation = conversationQuery(context);
            const auto documents = runKnowledge(
                [this, requester = identity.user.id, conversation]
                {
                    return knowledge_.knowledgeDocuments(requester,
                                                          conversation);
                });
            reply(context, 200, documentsJson(documents));
        });
    });

    router.GET("/api/ai/generations/:generation/citations",
               [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto found = context.params.find("generation");
            if (found == context.params.end() || found->second.empty())
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "generation id is required");
            const auto citations = runKnowledge(
                [this, requester = identity.user.id,
                 generation = found->second]
                {
                    return knowledge_.generationCitations(requester,
                                                          generation);
                });
            reply(context, 200, citationsJson(citations));
        });
    });
}

} // namespace webserver::phase11::transport
