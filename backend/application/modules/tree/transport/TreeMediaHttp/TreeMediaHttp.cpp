#include "TreeMediaHttp.h"
namespace webserver::function3 {
void TreeMediaHttp::registerRoutes(Router&router){
    auto scope=http::executor(router,executor_);
    router.POST("/api/tree/media/uploads",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto actor=auth_.authorizeRequest(ctx,true).user.id;auto f=http::body(ctx);auto result=media_.create(actor,http::field(f,"name"),http::field(f,"mediaType"),http::number(http::field(f,"size")),std::chrono::system_clock::now());http::reply(ctx,"{\"id\":"+http::id(result.id)+"}",201);});});
    router.POST("/api/tree/media/:file/validate",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto actor=auth_.authorizeRequest(ctx,true).user.id;auto v=media_.validate(actor,http::path(ctx,"file"));http::reply(ctx,"{\"id\":"+http::id(v.media.file)+",\"duration\":"+std::to_string(v.media.duration)+"}");});});
    router.GET("/api/tree/posts/:post/media/:file",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto actor=auth_.authorizeRequest(ctx).user.id;auto value=media_.download(actor,http::path(ctx,"post"),http::path(ctx,"file"));ctx.response->setHeader("Content-Type",value.mime);ctx.response->setHeader("Cache-Control","private,no-store");ctx.response->setHeader("X-Content-Type-Options","nosniff");ctx.response->setHeader("Content-Disposition","inline");if(!ctx.response->sendfile(value.path,ctx.request,ctx.request.range))throw BusinessError(Failure::NotFound,"media bytes missing");});});
}
}
