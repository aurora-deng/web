#pragma once
#include "backend/server/Route/Router.h"
#include "backend/application/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"
#include "backend/application/transport/FlatJson/FlatJson.h"
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
#include <charconv>
#include <chrono>

namespace webserver::function3::http {
using Fields=phase11::transport::FlatStringObject;
inline std::string quote(std::string_view text){return phase11::transport::quoteJson(text);}
inline std::string id(Id value){return quote(std::to_string(value));}
inline Milliseconds now(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
inline Id number(std::string_view value,bool zero=false){
    Id result{};auto p=std::from_chars(value.data(),value.data()+value.size(),result);
    if(value.empty()||p.ec!=std::errc{}||p.ptr!=value.data()+value.size()||(!zero&&!result)||result>9223372036854775807ULL)throw BusinessError(Failure::Invalid,"invalid decimal resource id");return result;
}
inline Id path(const RequestContext &ctx,std::string name){return number(ctx.params.at(name));}
inline std::string query(const RequestContext &ctx,std::string name,std::string fallback={}){auto i=ctx.request.querryParams.find(name);return i==ctx.request.querryParams.end()?fallback:i->second;}
inline std::string field(const Fields &fields,std::string name,std::string fallback={}){auto i=fields.find(name);return i==fields.end()?fallback:i->second;}
inline bool boolean(std::string_view value){if(value=="true")return true;if(value=="false")return false;throw BusinessError(Failure::Invalid,"boolean must be string true or false");}
inline Fields body(const RequestContext &ctx){
    if(ctx.request.bodyData.size()>32768)throw BusinessError(Failure::Invalid,"JSON body exceeds 32 KiB");
    auto c=ctx.request.headers.find("content-type");
    if(c==ctx.request.headers.end()||!c->second.starts_with("application/json"))throw BusinessError(Failure::Invalid,"application/json required");
    try{return phase11::transport::parseFlatStringObject(ctx.request.bodyData);}catch(const std::invalid_argument&){throw BusinessError(Failure::Invalid,"flat string JSON object required");}
}
inline void reply(RequestContext &ctx,std::string json,int status=200){ctx.response->status=status;ctx.response->statusText=status==200?"OK":status==201?"Created":status==202?"Accepted":"Error";ctx.response->json(json);ctx.response->setHeader("Cache-Control","no-store");ctx.response->setHeader("X-Content-Type-Options","nosniff");}
template<class Operation> bool guarded(RequestContext &ctx,Operation operation){
    try{if(ctx.stopRequested())throw BusinessError(Failure::Busy,"request cancelled");operation();}
    catch(const BusinessError &e){int status=500;switch(e.code){case Failure::Invalid:status=400;break;case Failure::Unauthenticated:status=401;break;case Failure::Forbidden:status=403;break;case Failure::NotFound:status=404;break;case Failure::Conflict:status=409;break;case Failure::Busy:status=503;break;}reply(ctx,"{\"message\":"+quote(e.what())+"}",status);}
    catch(const phase11::ApplicationError &e){int status=503;switch(e.code()){case phase11::ErrorCode::Unauthenticated:status=401;break;case phase11::ErrorCode::Forbidden:status=403;break;case phase11::ErrorCode::InvalidArgument:status=400;break;case phase11::ErrorCode::NotFound:status=404;break;case phase11::ErrorCode::Conflict:status=409;break;case phase11::ErrorCode::RateLimited:status=429;break;default:break;}reply(ctx,"{\"message\":"+quote(e.what())+"}",status);}
    catch(const std::exception &){reply(ctx,"{\"message\":\"internal server error\"}",500);}return true;
}
inline HandlerExecutorScope executor(Router &router,phase11::DatabaseExecutor &pool){return router.withExecutor([&pool](auto job){return pool.submit(std::move(job));});}
template<class Collection,class Render> std::string array(const Collection &values,Render render){std::string result="[";for(auto &v:values){if(result.size()>1)result+=",";result+=render(v);}return result+"]";}
}
