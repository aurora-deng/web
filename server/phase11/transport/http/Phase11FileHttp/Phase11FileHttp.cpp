#include "server/phase11/transport/http/Phase11FileHttp/Phase11FileHttp.h"

#include "server/Route/Router.h"
#include "server/http/RequestContext/RequestContext.h"
#include "server/phase11/business/FileService/FileService.h"
#include "server/phase11/transport/FlatJson/FlatJson.h"
#include "server/phase11/transport/serialization/MessageJson/MessageJson.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <stdexcept>

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
    catch (const ApplicationError &error)
    {
        const auto [status, code] = mapError(error.code());
        reply(context, status, "{\"code\":" + quoteJson(code) +
              ",\"message\":" + quoteJson(error.what()) + "}");
    }
    catch (const std::exception &)
    {
        reply(context, 500,
              "{\"code\":\"internal\",\"message\":\"internal server error\"}");
    }
    return true;
}

FlatStringObject jsonBody(const RequestContext &context)
{
    if (context.request.bodyData.size() > 16 * 1024)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "JSON request body is too large");
    const auto type = context.request.headers.find("content-type");
    if (type == context.request.headers.end() ||
        !type->second.starts_with("application/json"))
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "Content-Type must be application/json");
    try { return parseFlatStringObject(context.request.bodyData); }
    catch (const std::invalid_argument &error)
    {
        throw ApplicationError(ErrorCode::InvalidArgument, error.what());
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

std::uint64_t unsignedValue(std::string_view value, std::string_view name)
{
    if (value.empty())
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid unsigned field: " + std::string(name));
    std::size_t consumed = 0;
    unsigned long long parsed = 0;
    try { parsed = std::stoull(std::string(value), &consumed); }
    catch (...)
    {
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid unsigned field: " + std::string(name));
    }
    if (consumed != value.size())
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid unsigned field: " + std::string(name));
    return parsed;
}

std::uint64_t pathId(const RequestContext &context, std::string_view name)
{
    const auto found = context.params.find(std::string(name));
    if (found == context.params.end())
        throw ApplicationError(ErrorCode::InvalidArgument, "missing path id");
    const auto value = unsignedValue(found->second, name);
    if (!value) throw ApplicationError(ErrorCode::InvalidArgument,
                                       "path id must be positive");
    return value;
}

std::string stateName(FileState state)
{
    switch (state)
    {
    case FileState::Pending: return "pending";
    case FileState::Ready: return "ready";
    case FileState::Failed: return "failed";
    case FileState::Deleted: return "deleted";
    }
    return "unknown";
}

std::string fileJson(const FileObject &file)
{
    return "{\"id\":" + std::to_string(file.id) +
           ",\"ownerId\":" + std::to_string(file.ownerId) +
           ",\"name\":" + quoteJson(file.originalName) +
           ",\"mediaType\":" + quoteJson(file.mediaType) +
           ",\"expectedSize\":" + std::to_string(file.expectedSize) +
           ",\"uploadedSize\":" + std::to_string(file.uploadedSize) +
           ",\"sha256\":" + quoteJson(file.sha256) +
           ",\"state\":" + quoteJson(stateName(file.state)) +
           ",\"url\":" + quoteJson("/api/files/" + std::to_string(file.id)) +
           "}";
}

std::string percentEncode(std::string_view input)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string output;
    for (const unsigned char c : input)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') output.push_back(c);
        else
        {
            output.push_back('%');
            output.push_back(digits[c >> 4]);
            output.push_back(digits[c & 0x0f]);
        }
    }
    return output;
}
} // namespace

void Phase11FileHttp::registerRoutes(Router &router)
{
    router.POST("/api/files/uploads", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto fields = jsonBody(context);
            const auto file = runFile([this, user = identity.user.id,
                                       name = required(fields, "name"),
                                       type = required(fields, "mediaType"),
                                       size = unsignedValue(required(fields, "size"), "size")]
            { return files_.createUpload(user, name, type, size, now()); });
            reply(context, 201, fileJson(file));
        });
    });

    router.PATCH("/api/files/:file/content", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            if (context.request.bodyData.size() > FileService::kMaximumChunkBytes)
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "chunk exceeds 256 KiB");
            const auto offsetHeader = context.request.headers.find("upload-offset");
            if (offsetHeader == context.request.headers.end())
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "Upload-Offset header is required");
            const auto fileId = pathId(context, "file");
            const auto offset = unsignedValue(offsetHeader->second, "Upload-Offset");
            const auto bytes = context.request.bodyData;
            const auto file = runFile([this, user = identity.user.id, fileId,
                                       offset, bytes]
            { return files_.appendChunk(user, fileId, offset, bytes); });
            reply(context, 200, fileJson(file));
            context.response->setHeader("Upload-Offset",
                                        std::to_string(file.uploadedSize));
        });
    });

    router.POST("/api/files/:file/complete", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto fileId = pathId(context, "file");
            const auto file = runFile([this, user = identity.user.id, fileId]
            { return files_.completeUpload(user, fileId, now()); });
            reply(context, 200, fileJson(file));
        });
    });

    router.POST("/api/me/avatar", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto fields = jsonBody(context);
            const auto fileId = unsignedValue(required(fields, "fileId"), "fileId");
            const auto user = runFile([this, owner = identity.user.id, fileId]
            { return files_.useAsAvatar(owner, fileId); });
            reply(context, 200,
                  "{\"avatarFileId\":" + std::to_string(*user.avatarFileId) +
                  ",\"avatarUrl\":" + quoteJson("/api/files/" +
                                                   std::to_string(*user.avatarFileId)) + "}");
        });
    });

    router.POST("/api/conversations/:conversation/file-messages",
                [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto conversation = pathId(context, "conversation");
            const auto fields = jsonBody(context);
            const auto clientId = required(fields, "clientMessageId");
            const auto fileId = unsignedValue(required(fields, "fileId"), "fileId");
            const auto caption = fields.contains("caption")
                                     ? fields.at("caption") : std::string{};
            const auto result = runFile([this, user = identity.user.id,
                                         conversation, clientId, caption, fileId]
            { return chat_.sendFile(user, conversation, clientId, caption,
                                    fileId, now()); });
            if (outboxCommitted_) outboxCommitted_();
            reply(context, result.inserted ? 201 : 200,
                  "{\"inserted\":" +
                      std::string(result.inserted ? "true" : "false") +
                      ",\"message\":" + serializeMessageJson(result.message) + "}");
        });
    });

    router.GET("/api/files/:file", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto fileId = pathId(context, "file");
            const auto download = runFile([this, user = identity.user.id, fileId]
            { return files_.download(user, fileId); });
            context.response->setHeader("Content-Type", download.file.mediaType);
            context.response->setHeader("X-Content-Type-Options", "nosniff");
            context.response->setHeader("Accept-Ranges", "bytes");
            context.response->setHeader("Cache-Control", "private,max-age=3600");
            const auto disposition = download.file.mediaType.starts_with("image/")
                                         ? "inline" : "attachment";
            context.response->setHeader(
                "Content-Disposition", std::string(disposition) +
                "; filename*=UTF-8''" + percentEncode(download.file.originalName));
            if (!context.response->sendfile(download.storagePath, context.request,
                                            context.request.range))
                throw ApplicationError(ErrorCode::NotFound, "file bytes are missing");
        });
    });
}

} // namespace webserver::phase11::transport
