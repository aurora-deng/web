#include "backend/application/transport/http/ConversationsHttp/ConversationsHttp.h"
#include "backend/application/transport/http/BusinessHttpCodec/BusinessHttpCodec.h"
#include "backend/server/Route/Router.h"
#include "backend/server/websocket/WebSocketSessionManager/WebSocketSessionManager.h"
namespace webserver::phase11::transport {
using namespace http_codec;
void ConversationsHttp::registerRoutes(Router &router) {
    auto execution=router.withExecutor([this](auto task){return databaseExecutor_.submit(std::move(task));});
    router.GET("/api/conversations", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto conversations = runDatabase(
                [this, user = identity.user.id]
                { return chat_.conversations(user); });
            reply(context, 200,
                  arrayJson(conversations, conversationSummaryJson));
        });
    });

    router.POST("/api/conversations", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto fields = bodyObject(context);
            const auto kind = required(fields, "kind");
            Conversation conversation;
            if (kind == "direct")
            {
                const auto peer = unsignedValue(required(fields, "peerId"), "peerId");
                conversation = runMutation(
                    [this, user = identity.user.id, peer]
                    { return chat_.createDirectConversation(user, peer, currentTime()); });
            }
            else if (kind == "group")
            {
                auto title = required(fields, "title");
                auto members = idList(required(fields, "memberIds"));
                conversation = runMutation(
                    [this, user = identity.user.id, title = std::move(title),
                     members = std::move(members)]() mutable
                    {
                        return chat_.createGroupConversation(
                            user, std::move(title), std::move(members), currentTime());
                    });
            }
            else
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "kind must be direct or group");
            reply(context, 201, conversationJson(conversation));
        });
    });

    // Agent确认卡/HTTP回退与WS复用同一消息事务和clientMessageId去重规则。

}
}
