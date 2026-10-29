#include "AdminHttp.h"
namespace webserver::function3 {
namespace {std::string roleName(Role role){return role==Role::Administrator?"administrator":role==Role::Moderator?"moderator":"user";}}
void AdminHttp::registerRoutes(Router &router){
    auto scope=http::executor(router,executor_);
    router.GET("/api/admin/me",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto actor=admin_.identity(auth_.authorizeRequest(ctx).user.id);http::reply(ctx,"{\"id\":"+http::id(actor.id)+",\"role\":"+http::quote(roleName(actor.role))+"}");});});
    router.GET("/api/admin/users",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto users=admin_.users(auth_.authorizeRequest(ctx).user.id,http::number(http::query(ctx,"after","0"),true));http::reply(ctx,http::array(users,[](const auto&u){return "{\"id\":"+http::id(u.actor.id)+",\"name\":"+http::quote(u.actor.name)+",\"username\":"+http::quote(u.username)+",\"role\":"+http::quote(roleName(u.actor.role))+",\"active\":"+(u.actor.active?"true":"false")+"}";}));});});
    router.POST("/api/admin/users/:user",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto actor=auth_.authorizeRequest(ctx,true).user.id;auto f=http::body(ctx);auto user=http::path(ctx,"user");auto action=http::field(f,"action"),reason=http::field(f,"reason");
        if(action=="role"){auto role=http::field(f,"role");if(role!="user"&&role!="moderator"&&role!="administrator")throw BusinessError(Failure::Invalid,"unsupported role");admin_.role(actor,user,role=="administrator"?Role::Administrator:role=="moderator"?Role::Moderator:Role::User,reason);}
        else if(action=="active")admin_.active(actor,user,http::boolean(http::field(f,"active")),reason);
        else if(action=="revoke")admin_.revoke(actor,user,reason);
        else throw BusinessError(Failure::Invalid,"unsupported account action");
        if(revoked_)revoked_(user);
        http::reply(ctx,"{\"ok\":true}");});});
    router.GET("/api/admin/audits",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto rows=admin_.audits(auth_.authorizeRequest(ctx).user.id,http::number(http::query(ctx,"before","0"),true));http::reply(ctx,http::array(rows,[](const auto&r){return "{\"id\":"+http::id(r.id)+",\"actorId\":"+http::id(r.actor)+",\"action\":"+http::quote(r.action)+",\"resource\":"+http::quote(r.resource)+",\"reason\":"+http::quote(r.reason)+",\"at\":"+http::quote(r.at)+"}";}));});});
    router.GET("/api/admin/reports",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto rows=moderation_.pending(auth_.authorizeRequest(ctx).user.id,http::number(http::query(ctx,"before","0"),true));http::reply(ctx,http::array(rows,[](const auto&r){return "{\"id\":"+http::id(r.id)+",\"postId\":"+http::id(r.post)+",\"reporterId\":"+http::id(r.reporter)+",\"reason\":"+http::quote(r.reason)+"}";}));});});
    router.POST("/api/admin/reports/:report/resolve",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto actor=auth_.authorizeRequest(ctx,true).user.id;auto f=http::body(ctx);moderation_.resolve(actor,http::path(ctx,"report"),http::boolean(http::field(f,"dismiss")),http::field(f,"reason"),http::now());http::reply(ctx,"{\"ok\":true}");});});
    router.POST("/api/admin/tree/posts/:post/hide",[this](RequestContext&ctx){return http::guarded(ctx,[&]{tree_.hide(auth_.authorizeRequest(ctx,true).user.id,http::path(ctx,"post"),http::field(http::body(ctx),"reason"),http::now());http::reply(ctx,"{\"ok\":true}");});});
}
}
