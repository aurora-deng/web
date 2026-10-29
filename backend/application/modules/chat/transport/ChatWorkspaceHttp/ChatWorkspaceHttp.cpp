#include "backend/application/modules/tree/domain/TreeContentPolicy/TreeContentPolicy.h"
#include "ChatWorkspaceHttp.h"
#include "backend/application/transport/serialization/MessageJson/MessageJson.h"
#include "backend/application/business/FileService/FileService.h"
namespace webserver::chat {
    namespace {
        namespace http=webserver::function3::http;
        std::string preferenceJson(const ConversationPreference&p){
            return "{\"conversationId\":"+http::id(p.conversation)+",\"notificationsMuted\":"+(p.muted?"true":"false")+",\"version\":"+http::id(p.version)+"}";
        }
        std::string messageJson(const Message&m){
            return phase11::transport::serializeMessageJson(m);
        }
    }
    void ChatWorkspaceHttp::registerRoutes(Router&router){
        auto scope=http::executor(router,executor_);
        router.GET("/api/users/search",[this](RequestContext&ctx){
            return http::guarded(ctx,[&]{
                const auto actor=auth_.authorizeRequest(ctx).user.id;
                const auto page=service_.users(actor,http::query(ctx,"q"),http::number(http::query(ctx,"after","0"),true),http::number(http::query(ctx,"limit","20")));
                http::reply(ctx,"{\"items\":"+http::array(page.items,[](const UserCard&u){
                    return "{\"id\":"+http::id(u.id)+",\"username\":"+http::quote(u.username)+",\"displayName\":"+http::quote(u.displayName)+",\"avatarUrl\":"+(u.avatar?http::quote("/api/files/"+std::to_string(*u.avatar)):"null")+",\"aiAccount\":"+(u.ai?"true":"false")+",\"friendship\":"+(u.friendship?"true":"false")+",\"pendingRequest\":"+(u.pending?"true":"false")+",\"canRequestFriendship\":"+(!u.friendship&&!u.pending?"true":"false")+"}";
                }
                )+",\"nextCursor\":"+http::id(page.nextCursor)+"}");
            }
            );
        }
        );
        router.GET("/api/search/messages/page",[this](RequestContext&ctx){
            return http::guarded(ctx,[&]{
                const auto actor=auth_.authorizeRequest(ctx).user.id;const auto page=service_.search(actor,http::number(http::query(ctx,"conversationId","0"),true),http::query(ctx,"q"),http::number(http::query(ctx,"before","0"),true),http::number(http::query(ctx,"limit","30")));
                http::reply(ctx,"{\"items\":"+http::array(page.items,messageJson)+",\"nextCursor\":"+http::id(page.nextCursor)+"}");
            }
            );
        }
        );
        router.GET("/api/conversations/:id/preferences",[this](RequestContext&ctx){
            return http::guarded(ctx,[&]{
                http::reply(ctx,preferenceJson(service_.preferences(auth_.authorizeRequest(ctx).user.id,http::path(ctx,"id"))));
            }
            );
        }
        );
        router.PATCH("/api/conversations/:id/preferences",[this](RequestContext&ctx){
            return http::guarded(ctx,[&]{
                const auto actor=auth_.authorizeRequest(ctx,true).user.id;const auto body=http::body(ctx);auto value=service_.setPreferences(actor,http::path(ctx,"id"),http::boolean(http::field(body,"notificationsMuted")),std::chrono::system_clock::now());if(committed_)committed_();http::reply(ctx,preferenceJson(value));
            }
            );
        }
        );
        router.GET("/api/conversations/:id/messages/:messageId/context",[this](RequestContext&ctx){
            return http::guarded(ctx,[&]{
                const auto value=service_.context(auth_.authorizeRequest(ctx).user.id,http::path(ctx,"id"),http::path(ctx,"messageId"),http::number(http::query(ctx,"radius","20")));http::reply(ctx,"{\"target\":"+messageJson(value.target)+",\"items\":"+http::array(value.items,messageJson)+"}");
            }
            );
        }
        );
        router.GET("/api/client-capabilities",[this](RequestContext&ctx){
            return http::guarded(ctx,[&]{
                auth_.authorizeRequest(ctx);using F=phase11::FileService;using P=webserver::function3::TreeContentPolicy;
                const auto types=http::array(F::allowedUploadTypes(),[](const auto&entry){
                    return "{\"extension\":"+http::quote(entry.first)+",\"mediaTypes\":"+http::array(entry.second,[](const auto&value){
                        return http::quote(value);
                    }
                    )+"}";
                }
                );
                http::reply(ctx,"{\"contractVersion\":\"1\",\"userSearch\":true,\"messageSearchPage\":true,\"messageContext\":true,\"conversationPreferences\":true,\"uploads\":{\"chat\":{\"maxBytes\":"+std::to_string(F::kMaximumFileBytes)+",\"imageMaxBytes\":"+std::to_string(F::kMaximumImageBytes)+",\"chunkBytes\":"+std::to_string(F::kMaximumChunkBytes)+",\"allowed\":"+types+"},\"avatar\":{\"maxBytes\":"+std::to_string(F::kMaximumImageBytes)+"},\"treeImage\":{\"maxBytes\":"+std::to_string(P::imageBytes)+",\"maxCount\":"+std::to_string(P::images)+",\"mediaTypes\":"+http::array(P::imageTypes,[](auto value){
                    return http::quote(value);
                }
                )+"},\"treeVoice\":{\"maxBytes\":"+std::to_string(P::voiceBytes)+",\"maxDurationMs\":"+std::to_string(P::voiceMilliseconds)+",\"mediaTypes\":"+http::array(P::voiceTypes,[](auto value){
                    return http::quote(value);
                }
                )+"}}}");
            }
            );
        }
        );
    }
}
