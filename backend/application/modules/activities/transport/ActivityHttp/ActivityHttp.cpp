#include "ActivityHttp.h"
#include "backend/application/modules/activities/transport/ActivityJson/ActivityJson.h"
#include <sstream>
namespace webserver::function3 {
namespace {
std::vector<std::string> split(const std::string&value,char separator) {
    std::istringstream stream(value);std::vector<std::string> result;
    for(std::string part;std::getline(stream,part,separator);)if(!part.empty())result.push_back(part);return result;
}
}
void ActivityHttp::registerRoutes(Router&router) {
    auto scope=http::executor(router,executor_);
    router.POST("/api/activities",[this](RequestContext&ctx){return http::guarded(ctx,[&]{
        const auto user=auth_.authorizeRequest(ctx,true).user.id;const auto f=http::body(ctx);
        PublishPost p;p.requestId=http::field(f,"requestId");p.body=http::field(f,"body");
        const auto destination=http::field(f,"destination","profile");
        if(destination!="profile"&&destination!="tree"&&destination!="both")throw BusinessError(Failure::Invalid,"invalid destination");
        p.treeChannel=destination!="profile";p.profileChannel=destination!="tree";
        p.visibility=http::field(f,"visibility",p.treeChannel?"members":"friends");
        p.category=http::number(http::field(f,"categoryId","0"),true);p.mood=http::field(f,"mood");p.tags=split(http::field(f,"tags"),'\n');
        for(const auto&id:split(http::field(f,"fileIds"),','))p.files.push_back(http::number(id));
        http::reply(ctx,activityJson(service_.publish(user,std::move(p),http::boolean(http::field(f,"publicConfirmed","false")),http::now())),201);
    });});
    router.GET("/api/activities",[this](RequestContext&ctx){return http::guarded(ctx,[&]{
        const auto user=auth_.authorizeRequest(ctx).user.id;
        http::reply(ctx,http::array(service_.profileActivities(user,http::number(http::query(ctx,"author",std::to_string(user))),
            http::number(http::query(ctx,"before","0"),true),http::query(ctx,"channel")=="tree",24),activityJson));
    });});
    router.GET("/api/activities/favorites",[this](RequestContext&ctx){return http::guarded(ctx,[&]{
        const auto page=service_.favorites(auth_.authorizeRequest(ctx).user.id,http::number(http::query(ctx,"before","0"),true),24);
        http::reply(ctx,"{\"items\":"+http::array(page.items,activityJson)+",\"nextCursor\":"+http::id(page.nextCursor)+"}");
    });});
    router.GET("/api/activities/:post",[this](RequestContext&ctx){return http::guarded(ctx,[&]{http::reply(ctx,activityJson(service_.get(auth_.authorizeRequest(ctx).user.id,http::path(ctx,"post"))));});});
    router.POST("/api/activities/:post/favorite",[this](RequestContext&ctx){return http::guarded(ctx,[&]{
        const auto user=auth_.authorizeRequest(ctx,true).user.id;const auto f=http::body(ctx);
        http::reply(ctx,activityJson(service_.favorite(user,http::path(ctx,"post"),http::boolean(http::field(f,"enabled")),http::now())));
    });});
    router.GET("/api/activities/:post/share",[this](RequestContext&ctx){return http::guarded(ctx,[&]{
        http::reply(ctx,"{\"path\":"+http::quote(service_.share(auth_.authorizeRequest(ctx).user.id,http::path(ctx,"post")))+",\"requiresLogin\":true}");
    });});
    auto profileJson=[](const PublicProfile&p){return "{\"id\":"+http::id(p.id)+",\"username\":"+http::quote(p.username)+",\"displayName\":"+http::quote(p.name)+
        ",\"biography\":"+http::quote(p.biography)+",\"avatarUrl\":"+http::quote(p.avatarUrl)+",\"self\":"+(p.self?"true":"false")+
        ",\"aiAccount\":"+(p.ai?"true":"false")+",\"showBiography\":"+(p.showBiography?"true":"false")+",\"showAvatar\":"+(p.showAvatar?"true":"false")+
        ",\"allowTreeFriendRequests\":"+(p.allowTreeFriendRequests?"true":"false")+",\"friendship\":"+(p.friendship&&!p.blocked?"true":"false")+
        ",\"canRequestFriendship\":"+(!p.self&&p.allowTreeFriendRequests&&!p.blocked&&!p.friendship&&!p.pendingRequest?"true":"false")+
        ",\"pendingRequest\":"+(p.pendingRequest?"true":"false")+",\"audience\":"+http::quote(p.self?"self":p.friendship&&!p.blocked?"friends":"tree-only")+"}";};
    router.GET("/api/profiles/:user",[this,profileJson](RequestContext&ctx){return http::guarded(ctx,[&]{
        http::reply(ctx,profileJson(service_.profile(auth_.authorizeRequest(ctx).user.id,http::path(ctx,"user"))));
    });});
    router.GET("/api/tree/posts/:post/author",[this,profileJson](RequestContext&ctx){return http::guarded(ctx,[&]{
        http::reply(ctx,profileJson(service_.treeAuthor(auth_.authorizeRequest(ctx).user.id,http::path(ctx,"post"))));
    });});
    // 只接受果实ID，作者ID由服务端解析；复用社交业务的黑名单、去重、事务与Outbox。
    router.POST("/api/tree/posts/:post/friend-request",[this](RequestContext&ctx){return http::guarded(ctx,[&]{
        const auto actor=auth_.authorizeRequest(ctx,true).user.id;
        const auto target=service_.treeFriendTarget(actor,http::path(ctx,"post"));
        const auto request=social_.requestFriendship(actor,target,std::chrono::system_clock::now());
        http::reply(ctx,"{\"id\":"+http::id(request.id)+",\"state\":\"pending\"}",201);
    });});
    router.PATCH("/api/me/profile-display",[this](RequestContext&ctx){return http::guarded(ctx,[&]{
        const auto user=auth_.authorizeRequest(ctx,true).user.id;const auto f=http::body(ctx);
        service_.configureProfile(user,http::boolean(http::field(f,"showBiography")),http::boolean(http::field(f,"showAvatar")),f.contains("allowTreeFriendRequests")?std::optional<bool>{http::boolean(http::field(f,"allowTreeFriendRequests"))}:std::nullopt);http::reply(ctx,"{\"ok\":true}");
    });});
}
}
