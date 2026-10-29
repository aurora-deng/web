#include "SpeechAdminHttp.h"
#include <cmath>
namespace webserver::function3 {
std::string SpeechAdminHttp::previewState(const std::string&voice,bool start,bool personal,double speed){
    if(voice!="zf_001"&&voice!="zm_009")throw BusinessError(Failure::Invalid,"unsupported preset voice");
    const auto stateKey=std::string(personal?"personal:":"site:")+voice;
    auto state=state_;std::lock_guard lock(state->mutex);
    auto found=state->voices.find(stateKey);
    if(start&&personal){if(!speed)speed=voice=="zf_001"?.95:.94;if(!std::isfinite(speed)||speed<.75||speed>1.25)throw BusinessError(Failure::Invalid,"speech speed exceeds range");if(found!=state->voices.end()&&found->second.state=="pending"&&std::abs(found->second.speed-speed)>.001)throw BusinessError(Failure::Busy,"another preview speed is being generated");}
    if(start&&(found==state->voices.end()||found->second.state!="pending")){
        state->voices[stateKey]={};const auto revision=++state->revision;state->voices[stateKey].revision=revision;state->voices[stateKey].speed=speed;auto*speech=&speech_;
        // HTTP 立即返回；模型加载由专用队列负责，排队/生成不延长请求生命周期。
        if(!executor_.submit([state,speech,voice,stateKey,revision,personal,speed]{
            Preview result;result.speed=speed;try{result.key=speech->preview(voice,personal,speed);result.state="ready";}catch(...){result.state="failed";}
            std::lock_guard guard(state->mutex);auto old=state->voices.find(stateKey);if(old!=state->voices.end()&&old->second.revision==revision)state->voices[stateKey]=std::move(result);
        })){state->voices.erase(stateKey);throw BusinessError(Failure::Busy,"speech preview queue full");}
        found=state->voices.find(stateKey);
    }
    if(found==state->voices.end())return "{\"state\":\"idle\"}";
    return "{\"state\":"+http::quote(found->second.state)+",\"speed\":"+std::to_string(found->second.speed)+",\"url\":"+http::quote(found->second.key.empty()?"":std::string(personal?"/api/me/speech/audio/":"/api/admin/speech/audio/")+found->second.key)+"}";
}
void SpeechAdminHttp::registerRoutes(Router&router){
    router.GET("/api/admin/speech/settings",[this](RequestContext&ctx){return http::guarded(ctx,[&]{
        auto actor=activeActor(actors_,auth_.authorizeRequest(ctx).user.id);if(actor.role!=Role::Administrator)throw BusinessError(Failure::Forbidden,"site administrator required");
        auto*c=speech_.control();if(!c){http::reply(ctx,"{\"available\":false,\"provider\":\"local\"}");return;}
        std::string profiles="[";for(const auto*voice:{"zf_001","zm_009"}){auto p=c->profile(voice);if(profiles.size()>1)profiles+=",";profiles+="{\"voice\":"+http::quote(p.voice)+",\"provider\":"+http::quote(nebula::speech::providerName(p.provider))+",\"style\":"+http::quote(p.style)+",\"region\":"+http::quote(p.region)+",\"speed\":"+std::to_string(p.speed)+",\"pitch\":"+std::to_string(p.pitchPercent)+",\"freeTierConfirmed\":"+(p.freeTierConfirmed?"true":"false")+",\"dataUseConsent\":"+(p.dataUseConsent?"true":"false")+"}";}profiles+="]";
        http::reply(ctx,"{\"available\":true,\"profiles\":"+profiles+",\"runtime\":"+c->status()+"}");
    });});
    router.PATCH("/api/admin/speech/settings",[this](RequestContext&ctx){return http::guarded(ctx,[&]{
        auto actor=activeActor(actors_,auth_.authorizeRequest(ctx,true).user.id);if(actor.role!=Role::Administrator)throw BusinessError(Failure::Forbidden,"site administrator required");
        auto*c=speech_.control();if(!c)throw BusinessError(Failure::Busy,"cloud speech driver unavailable");const auto fields=http::body(ctx);auto p=c->profile(http::field(fields,"voice"));
        const auto provider=http::field(fields,"provider");if(provider!="local"&&provider!="azure"&&provider!="gemini")throw BusinessError(Failure::Invalid,"unsupported provider");p.provider=provider=="azure"?nebula::speech::Provider::Azure:provider=="gemini"?nebula::speech::Provider::Gemini:nebula::speech::Provider::Local;
        p.style=http::field(fields,"style",p.style);p.region=http::field(fields,"region",p.region);
        try{std::size_t n{};auto speed=http::field(fields,"speed",std::to_string(p.speed));p.speed=std::stod(speed,&n);if(n!=speed.size())throw std::invalid_argument("speed");auto pitch=http::field(fields,"pitch",std::to_string(p.pitchPercent));p.pitchPercent=std::stoi(pitch,&n);if(n!=pitch.size())throw std::invalid_argument("pitch");}catch(...){throw BusinessError(Failure::Invalid,"invalid prosody number");}
        p.freeTierConfirmed=http::boolean(http::field(fields,"freeTierConfirmed","false"));p.dataUseConsent=http::boolean(http::field(fields,"dataUseConsent","false"));c->configure(p,actor.id);
        {std::lock_guard lock(state_->mutex);state_->voices.erase("site:"+p.voice);}http::reply(ctx,"{\"ok\":true}");
    });});
    // 所有登录用户可试听固定文案的两个预置音色；固定状态表和任务队列限制生成量。
    router.POST("/api/me/speech/preview",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auth_.authorizeRequest(ctx,true);auto body=http::body(ctx);double speed=0;auto value=http::field(body,"speed");if(!value.empty()){try{std::size_t n;speed=std::stod(value,&n);if(n!=value.size())throw std::invalid_argument("speed");}catch(...){throw BusinessError(Failure::Invalid,"invalid speech speed");}}http::reply(ctx,previewState(http::field(body,"voice"),true,true,speed));});});
    router.GET("/api/me/speech/preview/:voice",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auth_.authorizeRequest(ctx);http::reply(ctx,previewState(ctx.params.at("voice"),false,true));});});
    router.GET("/api/me/speech/audio/:key",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auth_.authorizeRequest(ctx);const auto key=ctx.params.at("key");bool allowed=false;{std::lock_guard lock(state_->mutex);for(const auto&[voice,p]:state_->voices)if(p.state=="ready"&&p.key==key)allowed=true;}if(!allowed)throw BusinessError(Failure::NotFound,"preview not found");ctx.response->setHeader("Content-Type","audio/wav");ctx.response->setHeader("Cache-Control","private,no-store");if(!ctx.response->sendfile(speech_.audioPath(key),ctx.request,ctx.request.range))throw BusinessError(Failure::NotFound,"preview cache expired");});});
    router.POST("/api/admin/speech/preview",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto user=auth_.authorizeRequest(ctx,true).user.id;moderator(activeActor(actors_,user));http::reply(ctx,previewState(http::field(http::body(ctx),"voice"),true));});});
    router.GET("/api/admin/speech/preview/:voice",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto user=auth_.authorizeRequest(ctx).user.id;moderator(activeActor(actors_,user));http::reply(ctx,previewState(ctx.params.at("voice"),false));});});
    router.GET("/api/admin/speech/audio/:key",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto user=auth_.authorizeRequest(ctx).user.id;moderator(activeActor(actors_,user));auto path=speech_.audioPath(ctx.params.at("key"));ctx.response->setHeader("Content-Type","audio/wav");ctx.response->setHeader("Cache-Control","private,no-store");if(!ctx.response->sendfile(path,ctx.request,ctx.request.range))throw BusinessError(Failure::NotFound,"preview cache expired");});});
}
}
