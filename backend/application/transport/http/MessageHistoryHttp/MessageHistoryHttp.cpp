#include "backend/application/transport/http/MessageHistoryHttp/MessageHistoryHttp.h"
#include "backend/application/transport/http/BusinessHttpCodec/BusinessHttpCodec.h"
#include "backend/server/Route/Router.h"
#include "backend/server/websocket/WebSocketSessionManager/WebSocketSessionManager.h"
namespace webserver::phase11::transport {
using namespace http_codec;
void MessageHistoryHttp::registerRoutes(Router &router) {
    auto execution=router.withExecutor([this](auto task){return databaseExecutor_.submit(std::move(task));});
    router.POST("/api/conversations/:id/messages", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto conversation = unsignedValue(context.param("id"), "conversationId");
            const auto fields = bodyObject(context);
            const auto result = runMutation([this, user=identity.user.id, conversation, fields]
            { return chat_.sendText(user, conversation, required(fields, "clientMessageId"), required(fields, "text"), currentTime()); });
            reply(context, 201, messageJson(result.message));
        });
    });
    router.GET("/api/conversations/:id/messages", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            auto recovery=historyBudget_.acquire(identity.user.id);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            const bool backwards = context.request.querryParams.contains("before");
            if (backwards && context.request.querryParams.contains("after"))
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "after and before cannot be combined");
            const auto cursor = optionalUnsigned(context, backwards ? "before" : "after", 0);
            const auto limit = optionalUnsigned(context, "limit", 100);
            const auto messages = runDatabase(
                [this, user = identity.user.id, conversation, cursor, limit, backwards]
                {
                    return backwards ? chat_.historyBefore(user, conversation, cursor, limit)
                                     : chat_.history(user, conversation, cursor, limit);
                });
            reply(context, 200, arrayJson(messages, messageJson));
        });
    });

    router.GET("/api/conversations/:id/messages/by-client-id", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto conversation = unsignedValue(context.param("id"), "conversationId");
            const auto found = context.request.querryParams.find("clientMessageId");
            if (found == context.request.querryParams.end())
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "clientMessageId is required");
            const auto message = runDatabase(
                [this, user = identity.user.id, conversation, id = found->second]
                { return chat_.ownMessage(user, conversation, id); });
            // 未找到返回 null；与权限错误区分，前端据此决定是否仍可重试原命令。
            reply(context, 200, message ? messageJson(*message) : "null");
        });
    });

    router.GET("/api/search/messages", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto conversation = optionalUnsigned(
                context, "conversationId", 0);
            const auto query = context.request.querryParams.find("query");
            if (query == context.request.querryParams.end())
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "query is required");
            const auto limit = optionalUnsigned(context, "limit", 50);
            const auto messages = runDatabase(
                [this, user = identity.user.id, conversation,
                 text = query->second, limit]
                { return chat_.search(user, conversation, text, limit); });
            reply(context, 200, arrayJson(messages, messageJson));
        });
    });
}
}
