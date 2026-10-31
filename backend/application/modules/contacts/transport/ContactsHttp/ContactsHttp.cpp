#include "backend/application/modules/contacts/transport/ContactsHttp/ContactsHttp.h"
#include "backend/application/transport/http/BusinessHttpCodec/BusinessHttpCodec.h"
#include "backend/server/Route/Router.h"
#include "backend/server/websocket/WebSocketSessionManager/WebSocketSessionManager.h"
namespace webserver::phase11::transport {
using namespace http_codec;
void ContactsHttp::registerRoutes(Router &router) {
    auto execution=router.withExecutor([this](auto task){return databaseExecutor_.submit(std::move(task));});
    router.GET("/api/friends", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto friends = runDatabase(
                [this, user = identity.user.id] { return social_.friends(user); });
            reply(context, 200, arrayJson(friends, [this](const auto &contact)
            {
                return contactJson(
                    contact, webSockets_.connectionCount(contact.user.id));
            }));
        });
    });

    router.PATCH("/api/friends/:id", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto contact = unsignedValue(context.param("id"), "contactId");
            const auto fields = bodyObject(context);
            auto remark = required(fields, "remark");
            const auto mutedValue = required(fields, "notificationsMuted");
            if (mutedValue != "true" && mutedValue != "false")
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "notificationsMuted must be true or false");
            const auto updated = runMutation(
                [this, owner = identity.user.id, contact,
                 remark = std::move(remark), muted = mutedValue == "true"]() mutable
                {
                    return social_.updateContact(owner, contact, std::move(remark),
                                                 muted, currentTime());
                });
            reply(context, 200, contactJson(
                updated, webSockets_.connectionCount(updated.user.id)));
        });
    });

    router.DEL("/api/friends/:id", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto contact = unsignedValue(context.param("id"), "contactId");
            runMutation([this, user = identity.user.id, contact]
            { social_.removeFriend(user, contact, currentTime()); });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    router.GET("/api/blocks", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto blocked = runDatabase(
                [this, user = identity.user.id] { return social_.blocked(user); });
            reply(context, 200, arrayJson(blocked, userJson));
        });
    });

    router.POST("/api/blocks/:id", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto target = unsignedValue(context.param("id"), "targetId");
            runMutation([this, user = identity.user.id, target]
            { social_.block(user, target, currentTime()); });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    router.DEL("/api/blocks/:id", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto target = unsignedValue(context.param("id"), "targetId");
            runMutation([this, user = identity.user.id, target]
            { social_.unblock(user, target, currentTime()); });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    Handler requestFriend = [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto fields = bodyObject(context);
            const auto receiver = unsignedValue(required(fields, "receiverId"),
                                                "receiverId");
            const auto request = runMutation([this, sender = identity.user.id,
                                              receiver]
            {
                return social_.requestFriendship(sender, receiver, currentTime());
            });
            reply(context, 201, friendRequestJson(request));
        });
    };
    router.POST("/api/friends", requestFriend);
    router.POST("/api/friend-requests", std::move(requestFriend));

    router.GET("/api/friend-requests", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto requests = runDatabase(
                [this, user = identity.user.id] { return social_.pending(user); });
            reply(context, 200, arrayJson(requests, friendRequestJson));
        });
    });

    router.POST("/api/friend-requests/:id", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto requestId = unsignedValue(context.param("id"), "requestId");
            const auto decision = required(bodyObject(context), "accept");
            if (decision != "true" && decision != "false")
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "accept must be true or false");
            const auto request = runMutation(
                [this, requestId, receiver = identity.user.id,
                 accept = decision == "true"]
                {
                    return social_.decideFriendship(
                        requestId, receiver, accept, currentTime());
                });
            reply(context, 200, friendRequestJson(request));
        });
    });


}
}
