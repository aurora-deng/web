#include "backend/ai/speech/transport/SpeechHttp/AudioResponse.h"
#include "SpeechHttp.h"
namespace webserver::function3 {
void SpeechHttp::registerRoutes(Router&router){
    auto scope=http::executor(router,executor_);
    router.POST("/api/announcements/:announcement/claim",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto user=auth_.authorizeRequest(ctx,true).user.id;http::reply(ctx,"{\"claimed\":"+std::string(announcements_.claimPlayback(user,http::path(ctx,"announcement"))?"true":"false")+"}");});});
    router.GET("/api/announcements/:announcement/audio",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto id=http::path(ctx,"announcement"),user=auth_.authorizeRequest(ctx).user.id;announcements_.requireVisible(user,id);auto voice=http::query(ctx,"voice","zf_001");auto audio=speech_.audio(id,voice);http::reply(ctx,"{\"state\":"+http::quote(audio.state)+",\"url\":"+http::quote("/api/announcements/"+std::to_string(id)+"/audio/content?voice="+voice)+"}");});});
    router.GET("/api/announcements/:announcement/audio/content",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto id=http::path(ctx,"announcement"),user=auth_.authorizeRequest(ctx).user.id;announcements_.requireVisible(user,id);auto audio=speech_.audio(id,http::query(ctx,"voice","zf_001"));if(audio.state!="ready")throw BusinessError(Failure::NotFound,"audio not ready");http::speechAudio(ctx,speech_.audioPath(audio.cacheKey));});});
}
}
