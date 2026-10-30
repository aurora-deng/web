#include "backend/application/transport/http/GroupGovernanceHttp/GroupGovernanceHttp.h"
#include "backend/application/transport/http/BusinessHttpCodec/BusinessHttpCodec.h"
#include "backend/server/Route/Router.h"
#include "backend/server/websocket/WebSocketSessionManager/WebSocketSessionManager.h"
namespace webserver::phase11::transport {
using namespace http_codec;
void GroupGovernanceHttp::registerRoutes(Router &router) {
    auto execution=router.withExecutor([this](auto task){return databaseExecutor_.submit(std::move(task));});
    router.GET("/api/conversations/:id/members", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            const auto members = runDatabase(
                [this, user = identity.user.id, conversation]
                { return chat_.members(user, conversation); });
            reply(context, 200, arrayJson(members, memberJson));
        });
    });

    router.POST("/api/conversations/:id/members", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            const auto member = unsignedValue(
                required(bodyObject(context), "memberId"), "memberId");
            runMutation([this, actor = identity.user.id, conversation, member]
            {
                chat_.addGroupMember(actor, conversation, member, currentTime());
            });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    router.PATCH("/api/conversations/:id", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            const auto fields = bodyObject(context);
            auto title = required(fields, "title");
            auto announcement = required(fields, "announcement");
            const auto updated = runMutation(
                [this, actor = identity.user.id, conversation,
                 title = std::move(title),
                 announcement = std::move(announcement)]() mutable
                {
                    return chat_.updateGroupProfile(
                        actor, conversation, std::move(title),
                        std::move(announcement), currentTime());
                });
            reply(context, 200, conversationJson(updated));
        });
    });

    router.POST("/api/conversations/:id/members/:memberId/role",
                [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            const auto member = unsignedValue(context.param("memberId"),
                                               "memberId");
            const auto value = required(bodyObject(context), "role");
            const auto role = value == "administrator" ? MemberRole::Administrator
                              : value == "member" ? MemberRole::Member
                              : throw ApplicationError(
                                    ErrorCode::InvalidArgument,
                                    "role must be member or administrator");
            runMutation([this, actor = identity.user.id, conversation,
                         member, role]
            {
                chat_.setGroupMemberRole(actor, conversation, member, role,
                                         currentTime());
            });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    router.POST("/api/conversations/:id/members/:memberId/mute",
                [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            const auto member = unsignedValue(context.param("memberId"),
                                               "memberId");
            const auto seconds = nonnegativeValue(
                required(bodyObject(context), "seconds"), "seconds");
            if (seconds > 30ULL * 24 * 60 * 60)
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "mute duration cannot exceed 30 days");
            const auto now = currentTime();
            const auto until = seconds == 0
                ? std::optional<TimePoint>{}
                : std::optional<TimePoint>{now + std::chrono::seconds(seconds)};
            runMutation([this, actor = identity.user.id, conversation,
                         member, until, now]
            {
                chat_.setGroupMemberMute(actor, conversation, member, until, now);
            });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    router.POST("/api/conversations/:id/members/:memberId/remove",
                [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            const auto member = unsignedValue(context.param("memberId"),
                                               "memberId");
            runMutation([this, actor = identity.user.id, conversation, member]
            {
                chat_.removeGroupMember(actor, conversation, member, currentTime());
            });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    router.POST("/api/conversations/:id/leave", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            runMutation([this, user = identity.user.id, conversation]
            {
                chat_.leaveGroup(user, conversation, currentTime());
            });
            reply(context, 200, "{\"ok\":true}");
        });
    });

    router.POST("/api/conversations/:id/dissolve", [this](RequestContext &context)
    {
        return guarded(context, [&]
        {
            const auto identity = authentication_.authorizeRequest(context, true);
            const auto conversation = unsignedValue(context.param("id"),
                                                     "conversationId");
            runMutation([this, actor = identity.user.id, conversation]
            {
                chat_.dissolveGroup(actor, conversation, currentTime());
            });
            reply(context, 200, "{\"ok\":true}");
        });
    });


}
}
