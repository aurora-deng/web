#include "PrivateGenerationHttp.h"
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
namespace webserver::phase11::transport {
namespace {
namespace http=webserver::function3::http;
void secure(const RequestContext&ctx){if(!ctx.transportSecure)throw ApplicationError(ErrorCode::Forbidden,"private inference requires HTTPS");}
std::string viewJson(const PrivateGenerationView&v){return "{\"id\":"+http::quote(v.id)+",\"state\":"+http::quote(v.state)+",\"error\":"+http::quote(v.error)+
    ",\"pieces\":"+http::array(v.pieces,[](const auto&p){return http::quote(p);})+",\"next\":"+std::to_string(v.next)+",\"bytes\":"+std::to_string(v.bytes)+
    ",\"firstTokenMicroseconds\":"+std::to_string(v.firstTokenMicroseconds)+",\"modelNodeId\":"+std::to_string(v.trace.nodeId.value_or(0))+
    ",\"modelVersionId\":"+std::to_string(v.trace.versionId.value_or(0))+",\"serverPersistsContent\":false}";}
}
void PrivateGenerationHttp::registerRoutes(Router&router){
    auto scope=http::executor(router,executor_);
    const std::string base=web_?"/api/ai/web/generations":"/api/ai/private/generations";
    router.POST(base,[this](RequestContext&ctx){return http::guarded(ctx,[&]{
        if(!web_)secure(ctx);const auto owner=auth_.authorizeRequest(ctx,true).user.id;
        if(ctx.request.bodyData.size()>128*1024)throw ApplicationError(ErrorCode::InvalidArgument,"private JSON request too large");
        auto contentType=ctx.request.headers.find("content-type");if(contentType==ctx.request.headers.end()||!contentType->second.starts_with("application/json"))throw ApplicationError(ErrorCode::InvalidArgument,"application/json required");
        FlatStringObject f;try{f=parseFlatStringObject(ctx.request.bodyData);}catch(const std::invalid_argument&){throw ApplicationError(ErrorCode::InvalidArgument,"flat string JSON required");}
        if(web_&&http::field(f,"nonConfidentialConsent","false")!="true")throw ApplicationError(ErrorCode::Forbidden,"请先确认网页对话会发送到服务器，勿发送保密信息");
        GenerationRequest request;request.requestId=http::field(f,"requestId");request.maxOutputTokens=http::number(http::field(f,"maxOutputTokens","512"));
        const auto count=http::number(http::field(f,"messageCount"));if(count>26)throw ApplicationError(ErrorCode::InvalidArgument,"too many private context messages");
        for(std::size_t i=0;i<count;i++)request.messages.push_back({http::field(f,"role"+std::to_string(i)),http::field(f,"content"+std::to_string(i))});
        http::reply(ctx,viewJson(service_.start(owner,http::number(http::field(f,"aiUserId")),std::move(request))),202);
    });});
    router.GET(base+"/:generation",[this](RequestContext&ctx){return http::guarded(ctx,[&]{
        if(!web_)secure(ctx);const auto owner=auth_.authorizeRequest(ctx).user.id;
        http::reply(ctx,viewJson(service_.poll(owner,ctx.param("generation"),http::number(http::query(ctx,"after","0"),true))));
    });});
    router.DEL(base+"/:generation",[this](RequestContext&ctx){return http::guarded(ctx,[&]{
        if(!web_)secure(ctx);const auto owner=auth_.authorizeRequest(ctx,true).user.id;
        if(!service_.cancel(owner,ctx.param("generation")))throw ApplicationError(ErrorCode::NotFound,"private generation unavailable");http::reply(ctx,"{\"ok\":true}");
    });});
}
}
