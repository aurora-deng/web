#include "AnnouncementHttp.h"
namespace webserver::function3 {
void AnnouncementHttp::registerRoutes(Router &router){
    auto scope=http::executor(router,executor_);
    router.GET("/api/announcements",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto rows=announcements_.recent(auth_.authorizeRequest(ctx).user.id,http::number(http::query(ctx,"after","0"),true));http::reply(ctx,http::array(rows,[](const auto&a){return "{\"id\":"+http::id(a.id)+",\"version\":"+http::id(a.version)+",\"text\":"+http::quote(a.text)+",\"createdAt\":"+std::to_string(a.createdAt)+",\"expiresAt\":"+std::to_string(a.expiresAt)+"}";}));});});
    router.GET("/api/tree/ranking",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auth_.authorizeRequest(ctx);auto cached=cache_?cache_->lookup("ranking"):CacheLookup{};if(cached.value){http::reply(ctx,*cached.value);return;}auto rank=rank_.latest();auto json="{\"version\":"+http::id(rank.version)+",\"settledAt\":"+std::to_string(rank.settledAt)+",\"entries\":"+http::array(rank.entries,[](const auto&e){return "{\"postId\":"+http::id(e.post)+",\"score\":"+std::to_string(e.score)+"}";})+"}";if(cache_)cache_->store("ranking",cached.generation,json,2);http::reply(ctx,std::move(json));});});
    router.GET("/api/me/notification-settings",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto value=announcements_.settings(auth_.authorizeRequest(ctx).user.id);http::reply(ctx,"{\"sound\":"+std::string(value.sound?"true":"false")+",\"voice\":"+http::quote(value.voice)+"}");});});
    router.PATCH("/api/me/notification-settings",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto actor=auth_.authorizeRequest(ctx,true).user.id;auto f=http::body(ctx);announcements_.settings(actor,{http::boolean(http::field(f,"sound")),http::field(f,"voice")});http::reply(ctx,"{\"ok\":true}");});});
}
}
