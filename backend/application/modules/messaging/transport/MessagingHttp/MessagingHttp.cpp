#include "MessagingHttp.h"
namespace webserver::function3 {
void MessagingHttp::registerRoutes(Router&router){auto scope=http::executor(router,executor_);
    router.GET("/api/admin/messaging/consumer-failures",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto rows=service_.failures(auth_.authorizeRequest(ctx).user.id,http::number(http::query(ctx,"before","0"),true));http::reply(ctx,http::array(rows,[](auto&r){return "{\"id\":"+http::id(r.id)+",\"eventId\":"+http::id(r.event)+",\"topic\":"+http::quote(r.topic)+",\"key\":"+http::quote(r.key)+",\"attempts\":"+http::id(r.attempts)+",\"error\":"+http::quote(r.error)+"}";}));});});
    router.GET("/api/admin/messaging",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto rows=service_.status(auth_.authorizeRequest(ctx).user.id);http::reply(ctx,http::array(rows,[](auto&r){return "{\"state\":"+http::quote(r.state)+",\"count\":"+http::id(r.count)+",\"firstId\":"+http::id(r.first)+"}";}));});});
    router.GET("/api/admin/messaging/dead",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto rows=service_.dead(auth_.authorizeRequest(ctx).user.id,http::number(http::query(ctx,"before","0"),true));http::reply(ctx,http::array(rows,[](auto&r){return "{\"id\":"+http::id(r.id)+",\"type\":"+http::quote(r.type)+",\"aggregate\":"+http::quote(r.aggregate)+",\"aggregateId\":"+http::id(r.aggregateId)+",\"attempts\":"+http::id(r.attempts)+",\"error\":"+http::quote(r.error)+"}";}));});});
    router.POST("/api/admin/messaging/:event/replay",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto actor=auth_.authorizeRequest(ctx,true).user.id;service_.replay(actor,http::path(ctx,"event"),http::field(http::body(ctx),"reason"),http::now());http::reply(ctx,"{\"ok\":true}");});});
}
}
