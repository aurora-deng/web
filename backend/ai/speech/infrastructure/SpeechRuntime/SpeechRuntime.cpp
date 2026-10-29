#include "SpeechRuntime.h"
#include <cstdlib>
#ifdef NEBULA_HAS_CLOUD_SPEECH
#include "backend/ai/speech/infrastructure/SiteSpeechRenderer/SiteSpeechRenderer.h"
#endif
namespace webserver::function3 {
namespace {std::string env(const char*name,std::string fallback){const auto*p=std::getenv(name);return p&&*p?p:std::move(fallback);}}
SpeechRuntime::SpeechRuntime(SqlContext&sql,IEventWriter&events):tasks_(sql,&events),renderer_({env("WEB_FUNCTION3_TTS_WORKER",""),env("WEB_FUNCTION3_TTS_MODEL","/home/pikachu/phase11-models/kokoro-v1.1-zh"),env("WEB_FUNCTION3_TTS_ENGINE","/home/pikachu/phase11-deps/sherpa-onnx/1.13.8"),env("WEB_FUNCTION3_TTS_CACHE","/home/pikachu/phase11-data/function3/speech-cache"),"kokoro-v1.1-zh-int8:a1e94694776049035c4f2c6529f003aaece993c76aae9a78995831c3c4dcafc6"}),service_(tasks_,renderer_){
    if(env("WEB_FUNCTION3_TTS_WORKER","").empty())return;
    renderer_.verifyModel();
#ifdef NEBULA_HAS_CLOUD_SPEECH
    auto cloud=std::make_unique<SiteSpeechRenderer>(sql,renderer_);auto*control=cloud.get();
    cloudRenderer_=std::move(cloud);service_.setDriver(*cloudRenderer_,control);
#endif
    worker_=std::thread([this]{std::unique_lock lock(mutex_);while(!stopping_){lock.unlock();bool worked=false;try{worked=service_.runOne();}catch(...){}lock.lock();wake_.wait_for(lock,std::chrono::milliseconds(worked?10:500),[&]{return stopping_;});}});
}
void SpeechRuntime::stop(){{std::lock_guard lock(mutex_);stopping_=true;}renderer_.cancel();wake_.notify_all();if(worker_.joinable())worker_.join();}
SpeechRuntime::~SpeechRuntime(){stop();}
}
